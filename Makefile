include Makefile.inc

W=
EXT=elf
ifeq ($(OS),Windows_NT)
W=w
EXT=bin
endif

.PHONY: all
all: subdirs

MODE = run
SUBDIRS = kernel userapp

# uv 定位（qtest / full-l3 共用）。优先级必须是 USERPROFILE 在前：
# make 的 SHELL 是 MSYS bash，它把 HOME 覆盖成 /home/<user>（本仓 MSYS 安装无
# 此目录），只写 ${HOME:-...} 会永远命中错误值、uv.exe 找不到（实测见
# docs/evidence/2026-11-11/p10-uv-path-bug.log）。Linux 上 USERPROFILE 未定义，
# 自然退回 HOME，两边都正确。
UV = "$${USERPROFILE:-$${HOME}}/.local/bin/uv.exe"

.PHONY: subdirs $(SUBDIRS)
subdirs: $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@ $(MODE) CAPTURE_TEST=$(CAPTURE_TEST) CAP_REVERSE=$(CAP_REVERSE)

hd.img: subdirs
	if [ ! -s $@ ]; then cat $@.bz2.txt | base64 -d | bunzip2 >$@ ; fi
	mtools -c mcopy -i $@@@1M -D oO kernel/eposkrnl.$(EXT) ::eposkrnl
	mtools -c mcopy -i $@@@1M -D oO userapp/a.out ::
	mtools -c mcopy -i $@@@1M -D oO samples/*.wav ::

.PHONY: test
test:
	$(MAKE) -C tests test

# L2 qtest：不启动 OS，qtest 协议寄存器级验流与缓冲（docs/tech-plan-qtest.md）。
# D10：入口统一 make qtest，不用 watcher、不用独立 bat；uv 管理解释器。
.PHONY: qtest
qtest:
	$(UV) run --no-project python tests/qtest_client.py

# L3 整机回归：none 后端跑完整条音频栈（D10 的提交前档，tests/full_run.py）。
# 全程 QEMU_AUDIO_DRV=none —— 不碰宿主声卡（D9）、不动 QEMU 3.1.50（D8）。
# 不用 watcher、不用独立 bat（D10）：编排在 Python 侧轮询串口文件，QEMU 到点即 kill。
.PHONY: full-l3
full-l3:
	$(UV) run --no-project python tests/full_run.py

# 文档一致性：页面与实况不脱节（docs/decisions-docs.md D-D4，tools/check_docs.py）。
# 放在 full 首位——文档没对齐就不该谈测试全绿：一份写着「未开始」而实际已跑的
# 进度页，会让后续所有「已完成」都失去可信度。
.PHONY: docs-check
docs-check:
	$(UV) run --no-project python tools/check_docs.py

# 提交前全套：docs -> L1 -> L2 -> L3。
# 保持串行（不写 -j）是保守取舍：串行实测 29.6-30.6s、make clean 后 35.7s，均已满足
# D2 的 60s 全套预算，没有为省几秒去引入并行不确定性的收益。若将来要并行，先用
# docs/evidence/2026-11-11/p10-build-parallel-check.log 的方法实测再下结论。
.PHONY: full
full: docs-check test qtest full-l3

.PHONY: qemu
qemu: hd.img
	-qemu-system-i386$(W) -m 48 -boot order=c -vga std -drive format=raw,file=$^,index=0,media=disk -device intel-hda -device hda-duplex -serial file:userapp/serial.log

.PHONY: qemudbg
qemudbg: MODE=debug
qemudbg: hd.img
ifeq ($(OS),Windows_NT)
	-start $(GDB) -iex 'add-auto-load-safe-path .'
else
ifeq ($(shell uname -s),Linux)
	-/usr/bin/x-terminal-emulator -e $(GDB) -iex 'add-auto-load-safe-path .' &
endif
ifeq ($(shell uname -s),Darwin)
	-osascript -e 'on run argv' \
			   -e '  tell application "Terminal" to do script "cd $(shell pwd); $(GDB); exit"' \
			   -e 'end run'
endif
endif
	-qemu-system-i386$(W) -S -s -m 48 -boot order=c -vga std -drive format=raw,file=$^,index=0,media=disk -device intel-hda -device hda-duplex -serial file:userapp/serial.log

.PHONY: bochs
bochs: hd.img
ifeq ($(OS),Windows_NT)
	-bochs -q -f bochsrc-win.txt
else
	-bochs -q -f bochsrc-unix.txt
endif

.PHONY: bochsdbg
bochsdbg: MODE=debug
bochsdbg: hd.img
ifeq ($(OS),Windows_NT)
	-bochsdbg -q -f bochsrc-win.txt
else
	-bochsdbg -q -f bochsrc-unix.txt
endif

.PHONY: vbox
vbox: hd.vmdk
	@if ! VBoxManage -q list vms | grep epos >/dev/null; then \
		VBoxManage -q createvm --name epos --ostype Other --register; \
	else \
		VBoxManage -q controlvm epos poweroff; \
		sleep 1; \
		VBoxManage -q storagectl epos --name IDE --remove; \
		VBoxManage -q closemedium disk $^; \
	fi
	@VBoxManage -q modifyvm epos --cpus 1 --memory 48 --boot1 disk --nic1 bridged --nictype1 82540EM  --bridgeadapter1 en0 --macaddress1 auto
	@VBoxManage -q storagectl epos --add ide --name IDE
	@VBoxManage -q storageattach epos --storagectl IDE --port 0 --device 0 --type hdd --medium $^
	@VBoxManage startvm epos

.PHONY: tags
tags:
	ctags -R *

hd.vmdk: hd.img
	qemu-img convert -O vmdk $^ $@

.PHONY: run
run: qemu

.PHONY: debug
debug: qemudbg

.PHONY: clean
clean:
	@for subdir in $(SUBDIRS); do $(MAKE) -C $${subdir} $@; done
	$(RM) hd.img hd.vmdk tags .~* *.*~ ._* .DS_Store *.stackdump

.PHONY: diff
diff:
	@svn diff -x --ignore-eol-style | tr -d '\r' | gvim -u NONE -c "set titlestring=$(shell pwd)" \
																-c "set enc=utf-8" \
																-c "set go-=T" \
																-c "syn on" \
																-c "set ft=diff" -dmMnR - 2>&1 >/dev/null

.PHONY: submit
submit: clean
	@all=`svn status | grep '^[M?]' | cut -c9- | grep -v '^[[:punct:]]' | tr '\\\\' '/' | tr '\n' ' '`; \
	if [ -z "$${all}" ]; then echo ">>> No files changed."; exit; fi; \
	printf "%s\e[1;31m%s\e[0m\n" ">>> Files to be submitted: " "$${all}"; \
	size=`tar -cf- $${all} | bzip2 | wc -c | tr -d '[:space:]'`; \
	echo -n ">>> Total size of bytes $${size}"; \
	if [ $${size} -gt $$((64*1024)) ]; then echo " exceeds the limit of 64KiB."; exit; else echo ""; fi; \
	echo -n ">>> Enter student ID: "; \
	read sid; \
	if ! echo "$${sid}" | grep -qE '^[0-9]{8}$$'; then echo ">>> Student ID must be 8 digits."; exit; fi;\
	echo -n ">>> Enter password: "; \
	read passwd; \
	if [ -z "$${passwd}" ]; then echo ">>> Password cannot be empty."; exit; fi; \
	tar -cf- $${all} | bzip2 >$${sid}.tbz; \
	trap "rm $${sid}.tbz; exit" 1 2 3 15; \
	RESP=$$(curl -s -d "passwd=$${passwd}" "http://xgate.dhis.org:8080/osexp/verify.php?sid=$${sid}" | sed -n 's@<div .*>\(.*\)<\/div>@\1@p'); \
	if echo "$${RESP}" | grep -q '^Authorization: Bearer '; then \
		curl -s -H "$${RESP}" -F "myFile=@$${sid}.tbz" "http://xgate.dhis.org:8080/osexp/upload.php?sid=$${sid}" \
	    | sed -n 's@<div .*>\(.*\)<\/div>@>>> \1\'$$'\n@p'; \
	else \
		echo ">>> $${RESP}"; \
	fi; \
	$(RM) $${sid}.tbz;

