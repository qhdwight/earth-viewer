## Principles
- Prefer data-oriented design. Never object-oriented programming
- Write C-like C++ but prefer constexpr over #define. Macros are okay in moderation
- Think of programming as a sequence of transforms on data
- Fast compile times are important. Be concise but not unreadable
- Do not allocate memory after the program initialization. Frequently there is a static max size that most arrays can use
- Prefer highly linear and greppable code
- Never use any third-party library. This includes the standard C/C++ library besides random, atomic, cstdio, cstdint
- Platform-specific libraries like WIN32 and Vulkan are okay, but prefer the C version over C++
- No control flow is allowed inside macros
- Use const always when possible. Even for things not passed by reference
## Style
- For constants or enum entries, use lower camel case and prefix with k, i.e., kMyConstant
- For functions, use upper camel case, i.e., MyFunction
- For variables, use lower camel case, i.e., myVariable