#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <istream>
#include <streambuf>

namespace pldm
{

/**
 * @brief Minimal stream buffer for memory-mapped data
 */
class MmapStreamBuf : public std::streambuf
{
  private:
    char* dataStart;
    char* dataEnd;

    friend class MmapStream;

  public:
    /**
     * @brief Construct a stream buffer for memory-mapped data
     *
     * @param data Pointer to the memory-mapped data
     * @param size Size of the memory-mapped data in bytes
     */
    MmapStreamBuf(void* data, size_t size)
    {
        dataStart = static_cast<char*>(data);
        dataEnd = dataStart + size;
        setg(dataStart, dataStart, dataEnd);
    }

  protected:
    /**
     * @brief Seek to a position relative to a reference point
     *
     * @param offset Offset from the reference point
     * @param dir Direction/reference point (beginning, current, or end)
     * @param Open mode (unused, required for virtual function signature)
     * @return New position in the stream, or -1 on error
     */
    std::streampos seekoff(std::streamoff offset, std::ios_base::seekdir dir,
                           std::ios_base::openmode) override
    {
        char* newpos;

        switch (dir)
        {
            case std::ios_base::beg:
            {
                newpos = dataStart + offset;
                break;
            }
            case std::ios_base::cur:
            {
                newpos = gptr() + offset;
                break;
            }
            case std::ios_base::end:
            {
                newpos = dataEnd + offset;
                break;
            }
            default:
            {
                return -1;
            }
        }

        if (newpos < dataStart || newpos > dataEnd)
        {
            return -1;
        }

        setg(dataStart, newpos, dataEnd);
        return newpos - dataStart;
    }

    /**
     * @brief Seek to an absolute position in the stream
     *
     * @param pos Absolute position to seek to
     * @param mode Open mode (required for virtual function signature)
     * @return New position in the stream, or -1 on error
     */
    std::streampos seekpos(std::streampos pos,
                           std::ios_base::openmode mode) override
    {
        return seekoff(pos, std::ios_base::beg, mode);
    }
};

/**
 * @brief Stream wrapper for mmap'd data
 */
class MmapStream : public std::istream
{
  private:
    MmapStreamBuf buf;

  public:
    /**
     * @brief Construct an input stream for memory-mapped data
     *
     * @param data Pointer to the memory-mapped data
     * @param size Size of the memory-mapped data in bytes
     */
    MmapStream(void* data, size_t size) : std::istream(&buf), buf(data, size)
    {
        clear();
    }

    /** @brief Get the raw data pointer for the memory-mapped data
     *
     *  @return Raw pointer to the memory-mapped data
     */
    const uint8_t* data() const
    {
        return reinterpret_cast<const uint8_t*>(buf.dataStart);
    }

    /** @brief Get the size of the memory-mapped data
     *
     *  @return Size of the memory-mapped data in bytes
     */
    size_t size() const
    {
        return buf.dataEnd - buf.dataStart;
    }
};

/**
 * @brief Simple RAII wrapper for mmap
 */
class MmapFile
{
  private:
    void* mappedData = nullptr;
    size_t mappedSize = 0;
    int ownedFd = -1;

  public:
    MmapFile() = default;

    /**
     * @brief Map a file into memory
     *
     * @param fd File descriptor to map
     * @param ownsFd If true, this object takes ownership of the file descriptor
     * and will close it
     * @return true if mapping succeeded, false otherwise
     */
    bool map(int fd, bool ownsFd)
    {
        off_t fileSize = lseek(fd, 0, SEEK_END);
        if (fileSize <= 0)
        {
            return false;
        }

        lseek(fd, 0, SEEK_SET);

        mappedData = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mappedData == MAP_FAILED)
        {
            mappedData = nullptr;
            if (ownsFd)
            {
                close(fd);
            }
            return false;
        }

        mappedSize = fileSize;
        ownedFd = ownsFd ? fd : -1;
        return true;
    }

    /**
     * @brief Unmap the file from memory and close the file descriptor if owned
     */
    void unmap()
    {
        if (mappedData)
        {
            munmap(mappedData, mappedSize);
            mappedData = nullptr;
            mappedSize = 0;
        }
        if (ownedFd >= 0)
        {
            close(ownedFd);
            ownedFd = -1;
        }
    }

    ~MmapFile()
    {
        unmap();
    }

    /**
     * @brief Get the pointer to the memory-mapped data
     *
     * @return Pointer to the mapped data, or nullptr if not mapped
     */
    void* data() const
    {
        return mappedData;
    }

    /**
     * @brief Get the size of the memory-mapped data
     *
     * @return Size of the mapped data in bytes, or 0 if not mapped
     */
    size_t size() const
    {
        return mappedSize;
    }
};

} // namespace pldm
