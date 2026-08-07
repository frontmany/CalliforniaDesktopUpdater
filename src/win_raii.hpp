#pragma once

#include <windows.h>

#include <cstdio>
#include <utility>

namespace updater {

/// RAII owners for Win32 resources (styleguide: never manage HANDLEs by hand
/// at call sites). Move-only; copying would double-close.

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle) : mHandle(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : mHandle(std::exchange(other.mHandle, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            mHandle = std::exchange(other.mHandle, nullptr);
        }
        return *this;
    }
    ~UniqueHandle() { reset(); }

    [[nodiscard]] HANDLE get() const { return mHandle; }
    [[nodiscard]] bool valid() const { return mHandle != nullptr && mHandle != INVALID_HANDLE_VALUE; }

    void reset(const HANDLE handle = nullptr) {
        if (valid()) {
            CloseHandle(mHandle);
        }
        mHandle = handle;
    }

private:
    HANDLE mHandle{nullptr};
};

class UniqueRegKey {
public:
    UniqueRegKey() = default;
    UniqueRegKey(const UniqueRegKey&) = delete;
    UniqueRegKey& operator=(const UniqueRegKey&) = delete;
    ~UniqueRegKey() {
        if (mKey != nullptr) {
            RegCloseKey(mKey);
        }
    }

    [[nodiscard]] HKEY get() const { return mKey; }
    [[nodiscard]] HKEY* receive() { return &mKey; }
    [[nodiscard]] bool valid() const { return mKey != nullptr; }

private:
    HKEY mKey{nullptr};
};

class UniqueFile {
public:
    UniqueFile() = default;
    explicit UniqueFile(std::FILE* file) : mFile(file) {}
    UniqueFile(const UniqueFile&) = delete;
    UniqueFile& operator=(const UniqueFile&) = delete;
    ~UniqueFile() {
        if (mFile != nullptr) {
            std::fclose(mFile);
        }
    }

    [[nodiscard]] std::FILE* get() const { return mFile; }
    [[nodiscard]] bool valid() const { return mFile != nullptr; }

private:
    std::FILE* mFile{nullptr};
};

/// Owner for memory returned by CommandLineToArgvW and friends.
class UniqueLocalMemory {
public:
    UniqueLocalMemory() = default;
    explicit UniqueLocalMemory(void* memory) : mMemory(memory) {}
    UniqueLocalMemory(const UniqueLocalMemory&) = delete;
    UniqueLocalMemory& operator=(const UniqueLocalMemory&) = delete;
    ~UniqueLocalMemory() {
        if (mMemory != nullptr) {
            LocalFree(mMemory);
        }
    }

    [[nodiscard]] void* get() const { return mMemory; }

private:
    void* mMemory{nullptr};
};

}  // namespace updater
