# A folk of apple/llvm-project

For debug & study.

## build

```
$git clone https://github.com/chenxGen/llvm-project.git
$cd llvm-project
$cmake -S llvm -B build -G Xcode -DLLVM_ENABLE_PROJECTS="clang;libcxx;libcxxabi;clang-tools-extra"
# you may specify your C & C++ compiler path to work
#$cmake -S llvm -B build -G Xcode -DLLVM_ENABLE_PROJECTS="clang;libcxx;libcxxabi;clang-tools-extra" -D CMAKE_C_COMPILER='/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/cc' -D CMAKE_CXX_COMPILER='/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++'
```

## Debug case

- Header Search 
