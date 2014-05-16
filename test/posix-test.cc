/*
 Test wrappers around POSIX functions.

 Copyright (c) 2012-2014, Victor Zverovich
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "posix-test.h"

#include <errno.h>
#include <fcntl.h>

#include "gtest-extra.h"

namespace {
int open_count;
int close_count;
int dup_count;
int dup2_count;
int fdopen_count;
int fileno_count;
int read_count;
int write_count;
}

#define EMULATE_EINTR(func, error_result) \
  if (func##_count != 0) { \
    if (func##_count++ != 3) { \
      errno = EINTR; \
      return error_result; \
    } \
  }

#ifndef _WIN32
int test::open(const char *path, int oflag, int mode) {
  EMULATE_EINTR(open, -1);
  return ::open(path, oflag, mode);
}
#else
errno_t test::sopen_s(
    int* pfh, const char *filename, int oflag, int shflag, int pmode) {
  EMULATE_EINTR(open, EINTR);
  return _sopen_s(pfh, filename, oflag, shflag, pmode);
}
#endif

int test::close(int fildes) {
  // Close the file first because close shouldn't be retried.
  int result = ::close(fildes);
  EMULATE_EINTR(close, -1);
  return result;
}

int test::dup(int fildes) {
  EMULATE_EINTR(dup, -1);
  return ::dup(fildes);
}

int test::dup2(int fildes, int fildes2) {
  EMULATE_EINTR(dup2, -1);
  return ::dup2(fildes, fildes2);
}

FILE *test::fdopen(int fildes, const char *mode) {
  EMULATE_EINTR(fdopen, 0);
  return ::fdopen(fildes, mode);
}

int test::fileno(FILE *stream) {
  EMULATE_EINTR(fileno, -1);
  return ::fileno(stream);
}

test::ssize_t test::read(int fildes, void *buf, test::size_t nbyte) {
  EMULATE_EINTR(read, -1);
  return ::read(fildes, buf, nbyte);
}

test::ssize_t test::write(int fildes, const void *buf, test::size_t nbyte) {
  EMULATE_EINTR(write, -1);
  return ::write(fildes, buf, nbyte);
}

#ifndef _WIN32
# define EXPECT_RETRY(statement, func, message) \
    func##_count = 1; \
    statement; \
    EXPECT_EQ(4, func##_count); \
    func##_count = 0;
# define EXPECT_EQ_POSIX(expected, actual) EXPECT_EQ(expected, actual)
#else
# define EXPECT_RETRY(statement, func, message) \
    func##_count = 1; \
    EXPECT_SYSTEM_ERROR(statement, EINTR, message); \
    func##_count = 0;
# define EXPECT_EQ_POSIX(expected, actual)
#endif

TEST(FileTest, OpenRetry) {
  File *f = 0;
  EXPECT_RETRY(f = new File("CMakeLists.txt", File::RDONLY),
               open, "cannot open file CMakeLists.txt");
#ifndef _WIN32
  char c = 0;
  f->read(&c, 1);
#endif
  delete f;
}

TEST(FileTest, CloseNoRetryInDtor) {
  File read_end, write_end;
  File::pipe(read_end, write_end);
  File *f = new File(std::move(read_end));
  int saved_close_count = 0;
  EXPECT_WRITE(stderr, {
    close_count = 1;
    delete f;
    saved_close_count = close_count;
    close_count = 0;
  }, FormatSystemErrorMessage(EINTR, "cannot close file") + "\n");
  EXPECT_EQ(2, saved_close_count);
}

TEST(FileTest, CloseNoRetry) {
  File read_end, write_end;
  File::pipe(read_end, write_end);
  close_count = 1;
  EXPECT_SYSTEM_ERROR(read_end.close(), EINTR, "cannot close file");
  EXPECT_EQ(2, close_count);
  close_count = 0;
}

TEST(FileTest, ReadRetry) {
  File read_end, write_end;
  File::pipe(read_end, write_end);
  enum { SIZE = 4 };
  write_end.write("test", SIZE);
  write_end.close();
  char buffer[SIZE];
  std::streamsize count = 0;
  EXPECT_RETRY(count = read_end.read(buffer, SIZE),
      read, "cannot read from file");
  EXPECT_EQ_POSIX(SIZE, count);
}

TEST(FileTest, WriteRetry) {
  File read_end, write_end;
  File::pipe(read_end, write_end);
  enum { SIZE = 4 };
  std::streamsize count = 0;
  EXPECT_RETRY(count = write_end.write("test", SIZE),
      write, "cannot write to file");
  write_end.close();
#ifndef _WIN32
  EXPECT_EQ(SIZE, count);
  char buffer[SIZE + 1];
  read_end.read(buffer, SIZE);
  buffer[SIZE] = '\0';
  EXPECT_STREQ("test", buffer);
#endif
}

TEST(FileTest, DupRetry) {
  int stdout_fd = fileno(stdout);
  EXPECT_RETRY(File::dup(stdout_fd), dup,
      str(fmt::Format("cannot duplicate file descriptor {}") << stdout_fd));
}

TEST(FileTest, Dup2Retry) {
  File f1 = File::dup(fileno(stdout)), f2 = File::dup(fileno(stdout));
  EXPECT_RETRY(f1.dup2(f2.descriptor()), dup2,
      str(fmt::Format("cannot duplicate file descriptor {} to {}")
      << f1.descriptor() << f2.descriptor()));
}

// TODO: test retry on EINTR in dup2, pipe, fdopen
// TODO: test ConvertRWCount