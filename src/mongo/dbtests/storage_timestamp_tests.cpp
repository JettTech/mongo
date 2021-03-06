/**
 *    Copyright (C) 2017 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <cstdint>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class StorageTimestampTest {
public:
    ServiceContext::UniqueOperationContext _opCtxRaii = cc().makeOperationContext();
    OperationContext* _opCtx = _opCtxRaii.get();
    LogicalClock* _clock = LogicalClock::get(_opCtx);

    StorageTimestampTest() {
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        repl::ReplSettings replSettings;
        replSettings.setOplogSizeBytes(10 * 1024 * 1024);
        replSettings.setReplSetString("rs0");
        auto coordinatorMock =
            new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings);
        coordinatorMock->alwaysAllowWrites(true);
        setGlobalReplicationCoordinator(coordinatorMock);

        // Since the Client object persists across tests, even though the global
        // ReplicationCoordinator does not, we need to clear the last op associated with the client
        // to avoid the invariant in ReplClientInfo::setLastOp that the optime only goes forward.
        repl::ReplClientInfo::forClient(_opCtx->getClient()).clearLastOp_forTest();

        getGlobalServiceContext()->setOpObserver(stdx::make_unique<OpObserverImpl>());

        repl::setOplogCollectionName();
        repl::createOplog(_opCtx);

        ASSERT_OK(_clock->advanceClusterTime(LogicalTime(Timestamp(1, 0))));
    }

    ~StorageTimestampTest() {
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        try {
            reset(NamespaceString("local.oplog.rs"));
        } catch (...) {
            FAIL("Exception while cleaning up test");
        }
    }

    /**
     * Walking on ice: resetting the ReplicationCoordinator destroys the underlying
     * `DropPendingCollectionReaper`. Use a truncate/dropAllIndexes to clean out a collection
     * without actually dropping it.
     */
    void reset(NamespaceString nss) const {
        ::mongo::writeConflictRetry(_opCtx, "deleteAll", nss.ns(), [&] {
            invariant(_opCtx->recoveryUnit()->selectSnapshot(SnapshotName::min()).isOK());
            AutoGetCollection collRaii(_opCtx, nss, LockMode::MODE_X);

            if (collRaii.getCollection()) {
                WriteUnitOfWork wunit(_opCtx);
                invariant(collRaii.getCollection()->truncate(_opCtx).isOK());
                collRaii.getCollection()->getIndexCatalog()->dropAllIndexes(_opCtx, false);
                wunit.commit();
                return;
            }

            AutoGetOrCreateDb dbRaii(_opCtx, nss.db(), LockMode::MODE_X);
            WriteUnitOfWork wunit(_opCtx);
            invariant(dbRaii.getDb()->createCollection(_opCtx, nss.ns()));
            wunit.commit();
        });
    }

    void insertDocument(Collection* coll, const InsertStatement& stmt) {
        // Insert some documents.
        OpDebug* const nullOpDebug = nullptr;
        const bool enforceQuota = false;
        const bool fromMigrate = false;
        ASSERT_OK(coll->insertDocument(_opCtx, stmt, nullOpDebug, enforceQuota, fromMigrate));
    }

    std::int32_t itCount(Collection* coll) {
        std::uint64_t ret = 0;
        auto cursor = coll->getRecordStore()->getCursor(_opCtx);
        while (cursor->next() != boost::none) {
            ++ret;
        }

        return ret;
    }

    BSONObj findOne(Collection* coll) {
        auto optRecord = coll->getRecordStore()->getCursor(_opCtx)->next();
        ASSERT(optRecord != boost::none);
        return optRecord.get().data.toBson();
    }
};

class SecondaryInsertTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const std::uint32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            BSONObjBuilder result;
            ASSERT_OK(applyOps(
                _opCtx,
                nss.db().toString(),
                BSON("applyOps" << BSON_ARRAY(
                         BSON("ts" << firstInsertTime.addTicks(idx).asTimestamp() << "t" << 1LL
                                   << "h"
                                   << 0xBEEFBEEFLL
                                   << "v"
                                   << 2
                                   << "op"
                                   << "i"
                                   << "ns"
                                   << nss.ns()
                                   << "ui"
                                   << autoColl.getCollection()->uuid().get()
                                   << "o"
                                   << BSON("_id" << idx))
                         << BSON("ts" << firstInsertTime.addTicks(idx).asTimestamp() << "t" << 1LL
                                      << "h"
                                      << 1
                                      << "op"
                                      << "c"
                                      << "ns"
                                      << "test.$cmd"
                                      << "o"
                                      << BSON("applyOps" << BSONArrayBuilder().obj())))),
                &result));
        }

        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(
                SnapshotName(firstInsertTime.addTicks(idx).asTimestamp())));
            BSONObj result;
            ASSERT(Helpers::getLast(_opCtx, nss.ns().c_str(), result)) << " idx is " << idx;
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(result, BSON("_id" << idx)))
                << "Doc: " << result.toString() << " Expected: " << BSON("_id" << idx);
        }
    }
};

class SecondaryArrayInsertTimes : public StorageTimestampTest {
public:
    void run() {
        // Only run on 'wiredTiger'. No other storage engines to-date timestamp writes.
        if (mongo::storageGlobalParams.engine != "wiredTiger") {
            return;
        }

        // In order for applyOps to assign timestamps, we must be in non-replicated mode.
        repl::UnreplicatedWritesBlock uwb(_opCtx);

        // Create a new collection.
        NamespaceString nss("unittests.timestampedUpdates");
        reset(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X, LockMode::MODE_IX);

        const std::uint32_t docsToInsert = 10;
        const LogicalTime firstInsertTime = _clock->reserveTicks(docsToInsert);
        BSONObjBuilder fullCommand;
        BSONArrayBuilder applyOpsB(fullCommand.subarrayStart("applyOps"));

        BSONObjBuilder applyOpsElem1Builder;

        // Populate the "ts" field with an array of all the grouped inserts' timestamps.
        BSONArrayBuilder tsArrayBuilder(applyOpsElem1Builder.subarrayStart("ts"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            tsArrayBuilder.append(firstInsertTime.addTicks(idx).asTimestamp());
        }
        tsArrayBuilder.done();

        // Populate the "t" (term) field with an array of all the grouped inserts' terms.
        BSONArrayBuilder tArrayBuilder(applyOpsElem1Builder.subarrayStart("t"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            tArrayBuilder.append(1LL);
        }
        tArrayBuilder.done();

        // Populate the "o" field with an array of all the grouped inserts.
        BSONArrayBuilder oArrayBuilder(applyOpsElem1Builder.subarrayStart("o"));
        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            oArrayBuilder.append(BSON("_id" << idx));
        }
        oArrayBuilder.done();

        applyOpsElem1Builder << "h" << 0xBEEFBEEFLL << "v" << 2 << "op"
                             << "i"
                             << "ns" << nss.ns() << "ui" << autoColl.getCollection()->uuid().get();

        applyOpsB.append(applyOpsElem1Builder.done());

        BSONObjBuilder applyOpsElem2Builder;
        applyOpsElem2Builder << "ts" << firstInsertTime.addTicks(docsToInsert).asTimestamp() << "t"
                             << 1LL << "h" << 1 << "op"
                             << "c"
                             << "ns"
                             << "test.$cmd"
                             << "o" << BSON("applyOps" << BSONArrayBuilder().obj());

        applyOpsB.append(applyOpsElem2Builder.done());
        applyOpsB.done();
        // Apply the group of inserts.
        BSONObjBuilder result;
        ASSERT_OK(applyOps(_opCtx, nss.db().toString(), fullCommand.done(), &result));


        for (std::uint32_t idx = 0; idx < docsToInsert; ++idx) {
            auto recoveryUnit = _opCtx->recoveryUnit();
            recoveryUnit->abandonSnapshot();
            ASSERT_OK(recoveryUnit->selectSnapshot(
                SnapshotName(firstInsertTime.addTicks(idx).asTimestamp())));
            BSONObj result;
            ASSERT(Helpers::getLast(_opCtx, nss.ns().c_str(), result)) << " idx is " << idx;
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(result, BSON("_id" << idx)))
                << "Doc: " << result.toString() << " Expected: " << BSON("_id" << idx);
        }
    }
};

class AllStorageTimestampTests : public unittest::Suite {
public:
    AllStorageTimestampTests() : unittest::Suite("StorageTimestampTests") {}
    void setupTests() {
        add<SecondaryInsertTimes>();
        add<SecondaryArrayInsertTimes>();
    }
};

unittest::SuiteInstance<AllStorageTimestampTests> allStorageTimestampTests;
}  // namespace mongo
