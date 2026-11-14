/*
 * 01_double_free_bug.cpp — INTENTIONAL BUG: double free via a shallow copy (rule of 3 violated)
 *
 * Bug     : FrameBuffer owns a raw new[] buffer and has a destructor but no
 *           copy constructor. Passing it BY VALUE makes a shallow copy; both
 *           the copy and the original delete[] the same pointer.
 * Finds it: AddressSanitizer ("attempting double-free ... freed by thread T0
 *           here: ~FrameBuffer"); without ASan, glibc aborts with
 *           "free(): double free detected in tcache 2"; valgrind
 *           ("Invalid free() / delete / delete[]"). gdb on the abort: 'bt'.
 * Build   : make run P=dsa/03-debugging/cpp/01_double_free_bug.cpp   -> ASan double-free, exit 1
 * Read    : ../README.md  (section "Bug 7: double free")
 */
#include <cstddef>
#include <cstdio>

class FrameBuffer {
public:
    explicit FrameBuffer(std::size_t n) : size_(n), data_(new unsigned char[n]()) {}
    ~FrameBuffer() { delete[] data_; }
    // BUG: no copy constructor / copy assignment -> the compiler-generated
    // ones copy the POINTER. Fix: = delete them, deep-copy, or use std::vector.
    std::size_t size() const { return size_; }
private:
    std::size_t size_;
    unsigned char *data_;
};

static std::size_t report(FrameBuffer fb)   // BUG trigger: by value -> shallow copy
{
    return fb.size();
}                                           // copy destroyed here: delete[] #1

int main()
{
    FrameBuffer fb(1920 * 1080);
    std::printf("size = %zu\n", report(fb));
    return 0;
}                                           // original destroyed here: delete[] #2
