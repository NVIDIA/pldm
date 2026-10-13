#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>

namespace pldm
{

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
     * @param ownsFd If true, this object takes ownership of the file
     * descriptor and will close it. Ownership is honored on every failure
     * path too: when this returns false and ownsFd is true, fd has already
     * been closed and must not be closed again by the caller.
     * @return true if mapping succeeded, false otherwise
     */
    bool map(int fd, bool ownsFd)
    {
        off_t fileSize = lseek(fd, 0, SEEK_END);
        if (fileSize <= 0)
        {
            // Empty file or unseekable fd. Honor ownsFd here as well,
            // otherwise callers that hand over ownership leak the fd on
            // every zero-byte package.
            if (ownsFd)
            {
                close(fd);
            }
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
