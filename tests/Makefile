# 宿主侧 L1 单测入口。
#
# 真正的编译交给 run_tests.bat：本仓自带的 MSYS（e:/.../msys/2.0）没有 /usr/include，
# 而带 libc 的 C:\msys64\mingw64\bin\gcc.exe 从该 MSYS 的 sh 里启动会静默失败
# （exit 1、无任何诊断），从 cmd 启动才正常。所以 make 只负责转发。
#
# 复制 kernel/hda_capture.c 到 build/ 的原因：驱动用 `#include "kernel.h"` 引号形式，
# 引号包含先查「包含它的文件所在目录」，放在 build/ 里才能让 host/ 下的替身头生效。

.PHONY: test clean

test:
	cmd //c run_tests.bat

clean:
	rm -rf build
