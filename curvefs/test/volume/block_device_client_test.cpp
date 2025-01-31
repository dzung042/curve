/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/**
 * Project: Curve
 * Created Date: 2021-06-15
 * Author: Jingli Chen (Wine93)
 */

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include "absl/memory/memory.h"

#include "curvefs/src/volume/block_device_client.h"
#include "test/client/mock/mock_file_client.h"
#include "curvefs/test/volume/common.h"

namespace curvefs {
namespace volume {

using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::curve::client::UserInfo;
using ::curve::client::MockFileClient;
using AlignRead = std::pair<off_t, size_t>;
using AlignReads = std::vector<AlignRead>;

class BlockDeviceClientTest : public ::testing::Test {
 protected:
    void SetUp() override {
        options_.configPath = "/etc/curvefs/client.conf";
        options_.threadnum = 10;

        fileClient_ = std::make_shared<MockFileClient>();
        client_ = absl::make_unique<BlockDeviceClientImpl>(fileClient_);

        ON_CALL(*fileClient_, Init(_))
            .WillByDefault(Return(LIBCURVE_ERROR::OK));
        ASSERT_TRUE(client_->Init(options_));
    }

    void TearDown() override {}

    static ssize_t ReadCallback(int fd,
                                char* buf,
                                off_t offset,
                                size_t length) {
        for (auto i = 0; i < length; i++) {
            buf[i] = '1';
        }
        return length;
    }

 protected:
    BlockDeviceClientOptions options_;
    std::unique_ptr<BlockDeviceClientImpl> client_;
    std::shared_ptr<MockFileClient> fileClient_;
};

TEST_F(BlockDeviceClientTest, TestInit) {
    // CASE 1: init success
    EXPECT_CALL(*fileClient_, Init(options_.configPath))
        .WillOnce(Return(LIBCURVE_ERROR::OK));
    ASSERT_TRUE(client_->Init(options_));

    // CASE 2: init failed
    EXPECT_CALL(*fileClient_, Init(options_.configPath))
        .WillOnce(Return(LIBCURVE_ERROR::FAILED));
    ASSERT_FALSE(client_->Init(options_));
}

TEST_F(BlockDeviceClientTest, TestUnInit) {
    EXPECT_CALL(*fileClient_, UnInit())
        .Times(1);
    client_->UnInit();
}

TEST_F(BlockDeviceClientTest, TestOpen) {
    UserInfo userInfo("owner");

    // CASE 1: open return fd (-1)
    EXPECT_CALL(*fileClient_, Open(_, _, _))
        .WillOnce(Return(-1));
    ASSERT_FALSE(client_->Open("/filename", "owner"));

    // CASE 2: open return fd (0)
    EXPECT_CALL(*fileClient_, Open(_, _, _))
        .WillOnce(Return(0));
    ASSERT_TRUE(client_->Open("/filename", "owner"));

    // CASE 3: open return fd (1)
    EXPECT_CALL(*fileClient_, Open(_, _, _))
        .WillOnce(Return(10));
    ASSERT_TRUE(client_->Open("/filename", "owner"));
}

TEST_F(BlockDeviceClientTest, TestClose) {
    // CASE 1: close failed with file not open
    ASSERT_TRUE(client_->Close());

    // CASE 2: close failed
    EXPECT_CALL(*fileClient_, Open(_, _, _))
        .WillOnce(Return(10));
    ASSERT_TRUE(client_->Open("/filename", "owner"));

    EXPECT_CALL(*fileClient_, Close(10))
        .WillOnce(Return(-LIBCURVE_ERROR::FAILED));
    ASSERT_FALSE(client_->Close());

    // CASE 3: close success
    EXPECT_CALL(*fileClient_, Close(10))
        .WillOnce(Return(LIBCURVE_ERROR::OK));
    ASSERT_TRUE(client_->Close());
}

TEST_F(BlockDeviceClientTest, TestStat) {
    BlockDeviceStat stat;
    UserInfo userInfo("owner");

    // CASE 1: stat failed
    EXPECT_CALL(*fileClient_, StatFile("/filename", userInfo, _))
        .WillOnce(Return(-LIBCURVE_ERROR::FAILED));
    ASSERT_FALSE(client_->Stat("/filename", "owner", &stat));

    // CASE 2: stat success
    EXPECT_CALL(*fileClient_, StatFile("/filename", userInfo, _))
        .WillOnce(Invoke([](const std::string& filename,
                            const UserInfo& userinfo,
                            FileStatInfo* finfo) {
            finfo->length = 1000;
            finfo->fileStatus = 1;
            return LIBCURVE_ERROR::OK;
        }));
    ASSERT_TRUE(client_->Stat("/filename", "owner", &stat));
    ASSERT_EQ(stat.length, 1000);
    ASSERT_EQ(stat.status, BlockDeviceStatus::DELETING);
}

TEST_F(BlockDeviceClientTest, TestReadBasic) {
    char buf[4096];

    // CASE 1: read failed with file not open
    ASSERT_LT(client_->Read(buf, 0, 4096), 0);

    // CASE 2: read failed
    EXPECT_CALL(*fileClient_, Open(_, _, _))
        .WillOnce(Return(10));
    ASSERT_TRUE(client_->Open("/filename", "owner"));

    EXPECT_CALL(*fileClient_, Read(10, buf, 0, 4096))
        .WillOnce(Return(-1));
    ASSERT_LT(client_->Read(buf, 0, 4096), 0);

    // CASE 3: read failed with read not complete
    EXPECT_CALL(*fileClient_, Read(10, buf, 0, 4096))
        .WillOnce(Return(4095));
    ASSERT_LT(client_->Read(buf, 0, 4096), 0);

    // CASE 4: read success with length is zero
    EXPECT_CALL(*fileClient_, Read(_, _, _, _))
        .Times(0);
    ASSERT_EQ(client_->Read(buf, 0, 0), 0);

    // CASE 5: read success with aligned offset and length
    EXPECT_CALL(*fileClient_, Read(10, buf, 0, 4096))
        .WillOnce(Return(4096));
    ASSERT_EQ(client_->Read(buf, 0, 4096), 4096);
}

TEST_F(BlockDeviceClientTest, TestReadWithUnAligned) {
    auto TEST_READ = [this](off_t offset, size_t length,
                            off_t alignOffset, size_t alignLength) {
        char buf[40960];
        memset(buf, '0', sizeof(buf));

        EXPECT_CALL(*fileClient_, Read(10, _, alignOffset, alignLength))
            .WillOnce(Invoke(ReadCallback));

        ASSERT_GT(client_->Read(buf, offset, length), 0);
        for (auto i = 0; i < 40960; i++) {
            ASSERT_EQ(buf[i], i < length ? '1' : '0');
        }
    };

    // Prepare: open file
    EXPECT_CALL(*fileClient_, Open(_, _, _))
        .WillOnce(Return(10));
    ASSERT_TRUE(client_->Open("/filename", "owner"));

    // Test Cases: read success
    {
        TEST_READ(0, 1, 0, 4096);              // offset = 0,     length = 1
        TEST_READ(1, 4095, 0, 4096);           // offset = 1,     length = 4095
        TEST_READ(1, 4096, 0, 8192);           // offset = 1,     length = 4096
        TEST_READ(1000, 5000, 0, 8192);        // offset = 1000,  length = 5000
        TEST_READ(4096, 5000, 4096, 8192);     // offset = 4096,  length = 5000
        TEST_READ(10000, 10000, 8192, 12288);  // offset = 10000, length = 10000
    }

    // Test Cases: read failed
    {
        char buf[4096];
        memset(buf, '0', sizeof(buf));

        EXPECT_CALL(*fileClient_, Read(10, _, 0, 4096))
            .WillOnce(Return(0));
        ASSERT_LT(client_->Read(buf, 0, 1), 0);
        for (auto i = 0; i < 4096; i++) {
            ASSERT_EQ(buf[i], '0');
        }
    }
}

TEST_F(BlockDeviceClientTest, TestWriteBasic) {
    char buf[4096];

    // CASE 1: write failed with file not open
    ASSERT_LT(client_->Write(buf, 0, 4096), 0);

    // CASE 2: write failed
    EXPECT_CALL(*fileClient_, Open(_, _, _))
        .WillOnce(Return(10));
    ASSERT_TRUE(client_->Open("/filename", "owner"));

    EXPECT_CALL(*fileClient_, Write(10, buf, 0, 4096))
        .WillOnce(Return(-1));
    ASSERT_LT(client_->Write(buf, 0, 4096), 0);

    // CASE 3: write failed with write not complete
    EXPECT_CALL(*fileClient_, Write(10, buf, 0, 4096))
        .WillOnce(Return(4095));
    ASSERT_LT(client_->Write(buf, 0, 4096), 0);

    // CASE 4: write success with length is zero
    EXPECT_CALL(*fileClient_, Write(10, buf, 0, 4096))
        .Times(0);
    ASSERT_EQ(client_->Write(buf, 0, 0), 0);

    // CASE 5: write success with aligned offset and length
    EXPECT_CALL(*fileClient_, Write(10, buf, 0, 4096))
        .WillOnce(Return(4096));
    ASSERT_EQ(client_->Write(buf, 0, 4096), 4096);
}

TEST_F(BlockDeviceClientTest, TestWriteWithUnAligned) {
    auto TEST_WRITE = [this](off_t offset, size_t length,
                             off_t alignOffset, size_t alignLength,
                             AlignReads&& alignReads) {
        // Prepare write buffer
        char buf[40960], writeBuffer[40960];
        memset(buf, '0', sizeof(buf));
        memset(writeBuffer, '0', sizeof(writeBuffer));
        for (auto i = 0; i < length; i++) {
            buf[i] = '2';
        }

        // Align read
        for (auto& alignRead : alignReads) {
            auto readOffset = alignRead.first;
            auto readLength = alignRead.second;
            EXPECT_CALL(*fileClient_, Read(10, _, readOffset, readLength))
                .WillOnce(Invoke(ReadCallback));
        }

        // Align write
        EXPECT_CALL(*fileClient_, Write(10, _, alignOffset, alignLength))
            .WillOnce(Invoke([&](int fd, const char* buf,
                                 off_t offset, size_t length) {
                memcpy(writeBuffer, buf, length);
                return alignLength;
            }));

        ASSERT_GT(client_->Write(buf, offset, length), 0);

        // Check write buffer
        auto count = 0;
        for (auto i = 0; i < alignLength; i++) {
            auto pos = i + alignOffset;
            if (pos >= offset && pos < offset + length) {
                count++;
                ASSERT_EQ(writeBuffer[i], '2');
            } else {
                ASSERT_EQ(writeBuffer[i], '1');
            }
        }

        ASSERT_EQ(count, length);
    };

    // Prepare: open file
    EXPECT_CALL(*fileClient_, Open(_, _, _))
        .WillOnce(Return(10));
    ASSERT_TRUE(client_->Open("/filename", "owner"));

    // Test Cases: write success
    {
        TEST_WRITE(0, 1, 0, 4096, AlignReads{ AlignRead(0, 4096) });
        TEST_WRITE(1, 4095, 0, 4096, AlignReads{ AlignRead(0, 4096) });
        TEST_WRITE(1, 4096, 0, 8192, AlignReads{ AlignRead(0, 8192) });
        TEST_WRITE(1000, 5000, 0, 8192, AlignReads{ AlignRead(0, 8192) });
        TEST_WRITE(4096, 5000, 4096, 8192, AlignReads{ AlignRead(8192, 4096) });
        TEST_WRITE(10000, 10000, 8192, 12288,
                   AlignReads{ AlignRead(8192, 4096), AlignRead(16384, 4096) });
    }

    // Test Cases: write failed
    {
        char buf[4096];
        memset(buf, '0', sizeof(buf));

        // CASE 1: read failed -> write failed
        EXPECT_CALL(*fileClient_, Read(10, _, 0, 4096))
            .WillOnce(Return(-1));
        EXPECT_CALL(*fileClient_, Write(_, _, _, _))
            .Times(0);
        ASSERT_LT(client_->Write(buf, 0, 1), 0);

        // CASE 2: read unexpected bytes -> write failed
        EXPECT_CALL(*fileClient_, Read(10, _, 0, 8192))
            .WillOnce(Return(8191));
        EXPECT_CALL(*fileClient_, Write(_, _, _, _))
            .Times(0);
        ASSERT_LT(client_->Write(buf, 1000, 5000), 0);

        // CASE 3: read failed once -> write failed
        EXPECT_CALL(*fileClient_, Read(10, _, 8192, 4096))
            .WillOnce(Return(4096));
        EXPECT_CALL(*fileClient_, Read(10, _, 16384, 4096))
            .WillOnce(Return(4095));
        EXPECT_CALL(*fileClient_, Write(_, _, _, _))
            .Times(0);
        ASSERT_LT(client_->Write(buf, 10000, 10000), 0);

        // CASE 4: write failed
        EXPECT_CALL(*fileClient_, Read(10, _, 0, 4096))
            .WillOnce(Return(4096));
        EXPECT_CALL(*fileClient_, Write(_, _, _, _))
            .WillOnce(Return(-1));
        ASSERT_LT(client_->Write(buf, 0, 1), 0);
    }
}

TEST_F(BlockDeviceClientTest, ReadvTest_AllSuccess) {
    ON_CALL(*fileClient_, Open(_, _, _))
        .WillByDefault(Return(1));

    char data[4 * kKiB];

    std::vector<ReadPart> iov{
        { 0 * kMiB, 4 * kKiB, data},
        { 4 * kMiB, 4 * kKiB, data},
        { 8 * kMiB, 4 * kKiB, data},
        {12 * kMiB, 4 * kKiB, data},
    };

    EXPECT_CALL(*fileClient_, Read(_, _, _, _))
        .Times(4)
        .WillRepeatedly(
            Invoke([](int, char*, off_t, size_t length) { return length; }));

    ASSERT_TRUE(client_->Open({}, {}));
    ASSERT_EQ(4 * (4 * kKiB), client_->Readv(iov));
}

TEST_F(BlockDeviceClientTest, ReadvTest_AllFailed) {
    ON_CALL(*fileClient_, Open(_, _, _))
        .WillByDefault(Return(1));

    char data[4 * kKiB];

    std::vector<ReadPart> iov{
        { 0 * kMiB, 4 * kKiB, data},
        { 4 * kMiB, 4 * kKiB, data},
        { 8 * kMiB, 4 * kKiB, data},
        {12 * kMiB, 4 * kKiB, data},
    };

    EXPECT_CALL(*fileClient_, Read(_, _, _, _))
        .Times(4)
        .WillRepeatedly(
            Invoke([](int, char*, off_t, size_t length) { return -1; }));

    ASSERT_TRUE(client_->Open({}, {}));
    ASSERT_GT(0, client_->Readv(iov));
}

TEST_F(BlockDeviceClientTest, ReadvTest_PartialFailed) {
    ON_CALL(*fileClient_, Open(_, _, _))
        .WillByDefault(Return(1));

    char data[4 * kKiB];

    std::vector<ReadPart> iov{
        { 0 * kMiB, 4 * kKiB, data},
        { 4 * kMiB, 4 * kKiB, data},
        { 8 * kMiB, 4 * kKiB, data},
        {12 * kMiB, 4 * kKiB, data},
    };

    unsigned int seed = time(nullptr);
    int count = rand_r(&seed) % (iov.size() - 1) + 1;
    std::atomic<int> counter(iov.size());

    EXPECT_CALL(*fileClient_, Read(_, _, _, _))
        .Times(4)
        .WillRepeatedly(
            Invoke([&count, &counter](int, char*, off_t, size_t length) -> int {
                auto c = counter.fetch_sub(1);
                if (c <= count) {
                    return -1;
                }

                return length;
            }));

    ASSERT_TRUE(client_->Open({}, {}));
    ASSERT_GT(0, client_->Readv(iov));
}


TEST_F(BlockDeviceClientTest, WritevTest_AllSuccess) {
    ON_CALL(*fileClient_, Open(_, _, _))
        .WillByDefault(Return(1));

    char data[4 * kKiB];

    std::vector<WritePart> iov{
        { 0 * kMiB, 4 * kKiB, data},
        { 4 * kMiB, 4 * kKiB, data},
        { 8 * kMiB, 4 * kKiB, data},
        {12 * kMiB, 4 * kKiB, data},
    };

    EXPECT_CALL(*fileClient_, Write(_, _, _, _))
        .Times(4)
        .WillRepeatedly(Invoke(
            [](int, const char*, off_t, size_t length) { return length; }));

    ASSERT_TRUE(client_->Open({}, {}));
    ASSERT_EQ(4 * (4 * kKiB), client_->Writev(iov));
}

TEST_F(BlockDeviceClientTest, WritevTest_AllFailed) {
    ON_CALL(*fileClient_, Open(_, _, _))
        .WillByDefault(Return(1));

    char data[4 * kKiB];

    std::vector<WritePart> iov{
        { 0 * kMiB, 4 * kKiB, data},
        { 4 * kMiB, 4 * kKiB, data},
        { 8 * kMiB, 4 * kKiB, data},
        {12 * kMiB, 4 * kKiB, data},
    };

    EXPECT_CALL(*fileClient_, Write(_, _, _, _))
        .Times(4)
        .WillRepeatedly(
            Invoke([](int, const char*, off_t, size_t length) { return -1; }));

    ASSERT_TRUE(client_->Open({}, {}));
    ASSERT_GT(0, client_->Writev(iov));
}

TEST_F(BlockDeviceClientTest, WritevTest_PartialFailed) {
    ON_CALL(*fileClient_, Open(_, _, _))
        .WillByDefault(Return(1));

    char data[4 * kKiB];

    std::vector<WritePart> iov{
        { 0 * kMiB, 4 * kKiB, data},
        { 4 * kMiB, 4 * kKiB, data},
        { 8 * kMiB, 4 * kKiB, data},
        {12 * kMiB, 4 * kKiB, data},
    };

    unsigned int seed = time(nullptr);
    int count = rand_r(&seed) % (iov.size() - 1) + 1;
    std::atomic<int> counter(iov.size());

    EXPECT_CALL(*fileClient_, Write(_, _, _, _))
        .Times(4)
        .WillRepeatedly(Invoke(
            [&count, &counter](int, const char*, off_t, size_t length) -> int {
                auto c = counter.fetch_sub(1);
                if (c <= count) {
                    return -1;
                }

                return length;
            }));

    ASSERT_TRUE(client_->Open({}, {}));
    ASSERT_GT(0, client_->Writev(iov));
}

TEST_F(BlockDeviceClientTest, WritevTest_AllUnAlignedSuccess) {
    ON_CALL(*fileClient_, Open(_, _, _))
        .WillByDefault(Return(1));

    char data[4 * kKiB];

    std::vector<WritePart> iov{
        { 0 * kMiB, 2 * kKiB, data},
        { 4 * kMiB, 2 * kKiB, data},
        { 8 * kMiB, 2 * kKiB, data},
        {12 * kMiB, 2 * kKiB, data},
    };

    EXPECT_CALL(*fileClient_, Read(_, _, _, _))
        .Times(4)
        .WillRepeatedly(Invoke(
            [](int, const char*, off_t, size_t length) { return length; }));

    EXPECT_CALL(*fileClient_, Write(_, _, _, _))
        .Times(4)
        .WillRepeatedly(Invoke(
            [](int, const char*, off_t, size_t length) { return length; }));

    ASSERT_TRUE(client_->Open({}, {}));
    ASSERT_EQ(4 * (2 * kKiB), client_->Writev(iov));
}

}  // namespace volume
}  // namespace curvefs
