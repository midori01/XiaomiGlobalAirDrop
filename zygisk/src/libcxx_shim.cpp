// ABI shim: provide std::__ndk1::__hash_memory, which clang21's libc++ headers
// reference out-of-line but the older NDK r27 libc++_static.a does not define.
// Our unordered_maps are process-local and never interoperate, so any
// deterministic hash is correct. Use FNV-1a over the bytes.
#include <cstddef>

namespace std {
inline namespace __ndk1 {
__attribute__((visibility("default")))
size_t __hash_memory(const void *ptr, size_t size) noexcept {
    const unsigned char *p = static_cast<const unsigned char *>(ptr);
    size_t h = 1469598103934665603ULL;  // FNV offset basis (64-bit)
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;  // FNV prime
    }
    return h;
}
}  // namespace __ndk1
}  // namespace std
