#pragma once

#include <IO/ReadBufferFromFileBase.h>
#include <IO/MMappedFileDescriptor.h>


namespace DB
{

/** MMap range in a file and represent it as a ReadBuffer.
  * Please note that mmap is not always the optimal way to read file.
  * Also you cannot control whether and how long actual IO take place,
  *  so this method is not manageable and not recommended for anything except benchmarks.
  */
class MMapReadBufferFromFileDescriptor : public ReadBufferFromFileBase
{
public:
    off_t seek(off_t off, int whence) override;

protected:
    MMapReadBufferFromFileDescriptor() = default;
    void init();

    MMappedFileDescriptor mapped;

public:
    MMapReadBufferFromFileDescriptor(int fd_, size_t offset_, size_t length_);

    /// Map till end of file.
    MMapReadBufferFromFileDescriptor(int fd_, size_t offset_);

    /// unmap memory before call to destructor
    void finish();

    off_t getPosition() override;

    std::string getFileName() const override;

    size_t getFileOffsetOfBufferEnd() const override;

    int getFD() const;

    size_t getFileSize() override;

    size_t readBigAt(char * to, size_t n, size_t offset, const std::function<bool(size_t)> &) override;
    bool supportsReadAt() override { return true; }
};

}
