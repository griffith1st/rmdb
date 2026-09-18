#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>
#include <sys/wait.h>

#include "index/ix_manager.h"
#include "record/rm_manager.h"

#ifdef RMDB_TEST_IO_FAULTS
namespace {
int fault_fd = -1;
bool interrupt_write = false;
bool shorten_write = false;
int sync_calls = 0;
}
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_fdatasync(int);
extern "C" ssize_t __wrap_write(int fd, const void *buffer, size_t size) {
    if (fd == fault_fd && interrupt_write) {
        interrupt_write = false;
        errno = EINTR;
        return -1;
    }
    if (fd == fault_fd && shorten_write) size = std::min(size, size_t{3});
    return __real_write(fd, buffer, size);
}
extern "C" int __wrap_fdatasync(int fd) {
    if (fd == fault_fd) ++sync_calls;
    return __real_fdatasync(fd);
}
#endif

namespace {
class StorageRegression : public ::testing::Test {
protected:
    std::filesystem::path directory;

    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() / "rmdb-storage-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        directory = pattern;
    }

    void TearDown() override { std::filesystem::remove_all(directory); }
    std::string path(const char *name) const { return (directory / name).string(); }
};

TEST_F(StorageRegression, ExistenceChecksReleaseTheBufferFrame) {
    DiskManager disk;
    BufferPoolManager pool(1, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    Rid first = file->insert_record(data.data(), nullptr);
    for (int i = 1; i < file->get_file_hdr().num_records_per_page; ++i) {
        file->insert_record(data.data(), nullptr);
    }
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(file->is_record(first));
    EXPECT_NO_THROW(file->insert_record(data.data(), nullptr));
    records.close_file(file.get());
}

TEST_F(StorageRegression, ExistenceChecksRejectOutOfRangeRids) {
    DiskManager disk;
    BufferPoolManager pool(1, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    auto rid = file->insert_record(data.data(), nullptr);
    EXPECT_FALSE(file->is_record(Rid{rid.page_no, -1}));
    EXPECT_FALSE(file->is_record(Rid{rid.page_no, file->get_file_hdr().num_records_per_page}));
    EXPECT_FALSE(file->is_record(Rid{0, 0}));
    EXPECT_FALSE(file->is_record(Rid{file->get_file_hdr().num_pages, 0}));
    records.close_file(file.get());
}

TEST_F(StorageRegression, ExplicitInsertUnlinksAFullPageFromInsideTheFreeList) {
    DiskManager disk;
    BufferPoolManager pool(2, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    std::vector<Rid> rids;
    const int capacity = file->get_file_hdr().num_records_per_page;
    for (int i = 0; i < capacity * 2; ++i) rids.push_back(file->insert_record(data.data(), nullptr));
    file->delete_record(rids[0], nullptr);
    file->delete_record(rids[capacity], nullptr);
    file->insert_record(rids[0], data.data());
    file->insert_record(data.data(), nullptr);
    EXPECT_NO_THROW(file->insert_record(data.data(), nullptr));
    records.close_file(file.get());
}

TEST_F(StorageRegression, RecoveryInsertProvisionsMissingPages) {
    DiskManager disk;
    BufferPoolManager pool(1, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    data[0] = 'R';
    ASSERT_NO_THROW(file->insert_record(Rid{3, 2}, data.data()));
    EXPECT_EQ(file->get_file_hdr().num_pages, 4);
    EXPECT_EQ(file->get_record(Rid{3, 2}, nullptr)->data[0], 'R');
    // Every provisioned page must remain usable after reopen.
    records.close_file(file.get());
    file = records.open_file(path("records"));
    for (int i = 0; i < file->get_file_hdr().num_records_per_page * 3; ++i) {
        ASSERT_NO_THROW(file->insert_record(data.data(), nullptr));
    }
    records.close_file(file.get());
}

TEST_F(StorageRegression, ClosingARecordFileDoesNotPolluteAReusedDescriptor) {
    DiskManager disk;
    BufferPoolManager pool(4, &disk);
    RmManager records(&disk, &pool);
    for (const char *name : {"a", "b"}) {
        DiskManager setup_disk;
        BufferPoolManager setup_pool(1, &setup_disk);
        RmManager setup(&setup_disk, &setup_pool);
        setup.create_file(path(name), 1);
        auto file = setup.open_file(path(name));
        char value = name[0];
        file->insert_record(&value, nullptr);
        setup.close_file(file.get());
    }
    auto first = records.open_file(path("a"));
    EXPECT_EQ(first->get_record(Rid{1, 0}, nullptr)->data[0], 'a');
    const int fd = first->GetFd();
    records.close_file(first.get());
    auto second = records.open_file(path("b"));
    ASSERT_EQ(second->GetFd(), fd);
    EXPECT_EQ(second->get_record(Rid{1, 0}, nullptr)->data[0], 'b');
    records.close_file(second.get());
}

TEST_F(StorageRegression, ClosingAnIndexDoesNotPolluteAReusedDescriptor) {
    DiskManager disk;
    BufferPoolManager pool(4, &disk);
    IxManager indexes(&disk, &pool);
    ColMeta column;
    column.name = "key";
    column.type = TYPE_INT;
    column.len = sizeof(int);
    std::vector<ColMeta> columns{column};
    indexes.create_index(path("a"), columns);
    indexes.create_index(path("b"), columns);
    auto first = indexes.open_index(path("a"), columns);
    int first_fd = disk.get_file_fd(indexes.get_index_name(path("a"), columns));
    Page *page = pool.fetch_page(PageId{first_fd, IX_INIT_ROOT_PAGE});
    ASSERT_NE(page, nullptr);
    page->get_data()[PAGE_SIZE - 1] = 'a';
    pool.unpin_page(page->get_page_id(), true);
    indexes.close_index(first.get());
    auto second = indexes.open_index(path("b"), columns);
    int second_fd = disk.get_file_fd(indexes.get_index_name(path("b"), columns));
    ASSERT_EQ(first_fd, second_fd);
    page = pool.fetch_page(PageId{second_fd, IX_INIT_ROOT_PAGE});
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->get_data()[PAGE_SIZE - 1], 0);
    pool.unpin_page(page->get_page_id(), false);
    indexes.close_index(second.get());
}

TEST_F(StorageRegression, ConcurrentPageIoUsesIndependentOffsets) {
    DiskManager disk;
    disk.create_file(path("pages"));
    const int fd = disk.open_file(path("pages"));
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int page = 0; page < 8; ++page) {
        threads.emplace_back([&, page] {
            std::array<char, PAGE_SIZE> expected;
            expected.fill(static_cast<char>('A' + page));
            for (int i = 0; i < 200; ++i) {
                std::array<char, PAGE_SIZE> actual{};
                disk.write_page(fd, page, expected.data(), expected.size());
                disk.read_page(fd, page, actual.data(), actual.size());
                if (actual != expected) ++mismatches;
            }
        });
    }
    for (auto &thread : threads) thread.join();
    EXPECT_EQ(mismatches.load(), 0);
    disk.close_file(fd);
}

TEST_F(StorageRegression, NewPageMetadataSurvivesAnUnflushedProcessExit) {
    DiskManager disk;
    BufferPoolManager pool(1, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto file = records.open_file(path("records"));
        std::array<char, 512> data{};
        try {
            file->insert_record(data.data(), nullptr, [](const Rid &) {
                throw std::runtime_error("WAL unavailable");
            });
        } catch (const std::runtime_error &) {
            _exit(0);  // Simulate a process crash with the new page still buffered.
        }
        _exit(1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    for (int i = 0; i <= file->get_file_hdr().num_records_per_page; ++i) {
        EXPECT_NO_THROW(file->insert_record(data.data(), nullptr));
    }
    records.close_file(file.get());
}

TEST_F(StorageRegression, RecoveryRepairsAFreeListPublishedBeforeItsDeletePage) {
    DiskManager disk;
    BufferPoolManager pool(1, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    const int capacity = file->get_file_hdr().num_records_per_page;
    for (int i = 0; i < capacity; ++i) file->insert_record(data.data(), nullptr);
    records.close_file(file.get());
    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto reopened = records.open_file(path("records"));
        reopened->delete_record(Rid{1, 0}, nullptr);
        _exit(0);  // Header advertises page 1 as free; its persisted bitmap is still full.
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    file = records.open_file(path("records"));
    ASSERT_TRUE(file->is_record(Rid{1, 0}));
    file->rebuild_free_page_list();
    // Replay the committed delete, refill that slot, then allocate another page.
    file->delete_record(Rid{1, 0}, nullptr);
    EXPECT_EQ(file->insert_record(data.data(), nullptr).page_no, 1);
    EXPECT_NO_THROW(file->insert_record(data.data(), nullptr));
    records.close_file(file.get());
}

TEST_F(StorageRegression, RecoveryFindsAFreePageOmittedByAnUnflushedInsert) {
    DiskManager disk;
    BufferPoolManager pool(1, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    const int capacity = file->get_file_hdr().num_records_per_page;
    for (int i = 0; i < capacity - 1; ++i) file->insert_record(data.data(), nullptr);
    records.close_file(file.get());
    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        auto reopened = records.open_file(path("records"));
        reopened->insert_record(data.data(), nullptr);
        _exit(0);  // Header says no free pages, but the last inserted row never reached disk.
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    file = records.open_file(path("records"));
    // A loser insert is absent in the bitmap. Its slot must remain reusable.
    file->rebuild_free_page_list();
    EXPECT_EQ(file->insert_record(data.data(), nullptr).page_no, 1);
    EXPECT_EQ(file->get_file_hdr().num_pages, 2);
    records.close_file(file.get());
}

TEST_F(StorageRegression, InsertCallbackRunsBeforeTheRecordBecomesVisible) {
    DiskManager disk;
    BufferPoolManager pool(1, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    data[0] = 'L';
    bool invoked = false;
    Rid selected{};
    auto inserted = file->insert_record(data.data(), nullptr, [&](const Rid &rid) {
        invoked = true;
        selected = rid;
        EXPECT_FALSE(file->is_record(rid));
    });
    EXPECT_TRUE(invoked);
    EXPECT_EQ(inserted.page_no, selected.page_no);
    EXPECT_EQ(inserted.slot_no, selected.slot_no);
    EXPECT_EQ(file->get_record(inserted, nullptr)->data[0], 'L');
    records.close_file(file.get());
}

TEST_F(StorageRegression, FailedInsertCallbackLeavesNoRecordAndReleasesTheFrame) {
    DiskManager disk;
    BufferPoolManager pool(1, &disk);
    RmManager records(&disk, &pool);
    records.create_file(path("records"), 512);
    auto file = records.open_file(path("records"));
    std::array<char, 512> data{};
    Rid selected{};
    EXPECT_THROW(file->insert_record(data.data(), nullptr, [&](const Rid &rid) {
        selected = rid;
        throw std::runtime_error("WAL append failed");
    }), std::runtime_error);
    EXPECT_FALSE(file->is_record(selected));
    // One frame forces eviction after the first page fills; a leaked pin fails here.
    for (int i = 0; i <= file->get_file_hdr().num_records_per_page; ++i) {
        EXPECT_NO_THROW(file->insert_record(data.data(), nullptr));
    }
    records.close_file(file.get());
}

#ifdef RMDB_TEST_IO_FAULTS
TEST_F(StorageRegression, LogAppendRetriesInterruptedAndShortWrites) {
    DiskManager disk;
    disk.create_file(path("wal"));
    int fd = disk.open_file(path("wal"));
    disk.SetLogFd(fd);
    fault_fd = fd;
    interrupt_write = true;
    shorten_write = true;
    char payload[] = "durable transaction log";
    EXPECT_NO_THROW(disk.write_log(payload, sizeof(payload)));
    fault_fd = -1;
    interrupt_write = false;
    shorten_write = false;
    std::array<char, sizeof(payload)> actual{};
    EXPECT_EQ(pread(fd, actual.data(), actual.size(), 0), sizeof(payload));
    EXPECT_EQ(std::memcmp(actual.data(), payload, sizeof(payload)), 0);
    disk.close_file(fd);
}

TEST_F(StorageRegression, LogAppendSynchronizesBeforeReturning) {
    DiskManager disk;
    disk.create_file(path("wal"));
    int fd = disk.open_file(path("wal"));
    disk.SetLogFd(fd);
    fault_fd = fd;
    sync_calls = 0;
    char payload[] = "committed";
    disk.write_log(payload, sizeof(payload));
    EXPECT_GT(sync_calls, 0);
    fault_fd = -1;
    disk.close_file(fd);
}
#endif

TEST(RmRecordRegression, SelfAssignmentPreservesData) {
    std::array<char, 512> expected;
    expected.fill('x');
    RmRecord record(expected.size(), expected.data());
    record = record;
    EXPECT_EQ(std::memcmp(record.data, expected.data(), expected.size()), 0);
}

TEST(RmRecordRegression, RepeatedAssignmentAndDeserializationOwnTheirData) {
    char payload[] = "record";
    RmRecord source(sizeof(payload), payload);
    RmRecord destination(100);
    for (int i = 0; i < 100; ++i) destination = source;
    EXPECT_EQ(std::memcmp(destination.data, payload, sizeof(payload)), 0);
    std::array<char, sizeof(int) + sizeof(payload)> serialized{};
    const int size = sizeof(payload);
    std::memcpy(serialized.data(), &size, sizeof(int));
    std::memcpy(serialized.data() + sizeof(int), payload, sizeof(payload));
    RmRecord restored;
    restored.Deserialize(serialized.data());
    EXPECT_EQ(std::memcmp(restored.data, payload, sizeof(payload)), 0);
    // LeakSanitizer checks that both objects release all owned allocations.
}
}  // namespace
