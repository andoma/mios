GLOBALDEPS += ${T}support/version/gitver.mk

GITVER_VARGUARD = $(1)_GUARD_$(shell echo $($(1)) $($(2)) $($(3)) | ${MD5SUM} | cut -d ' ' -f 1)

GIT_DESC_MIOS_OUTPUT ?= $(shell cd "$(T)" && git describe --always --dirty=01 --abbrev=40 2>/dev/null)
GIT_DESC_APP_OUTPUT  ?= $(shell git describe --always --dirty=01 --abbrev=40 2>/dev/null)

VERSION_DIGEST := $(call GITVER_VARGUARD,GIT_DESC_MIOS_OUTPUT,GIT_DESC_APP_OUTPUT,APPNAME)

VERSION_DIGEST_STAMP := ${O}/.version_git/${VERSION_DIGEST}

${VERSION_DIGEST_STAMP}:
	rm -rf "${dir $@}"
	mkdir -p "${dir $@}"
	touch $@

MIOSVER := ${O}/versioninfo/miosver.bin
APPVER := ${O}/versioninfo/appver.bin

${MIOSVER}: ${VERSION_DIGEST_STAMP} ${GLOBALDEPS}
	mkdir -p "$(dir $@)"
	echo ${GIT_DESC_MIOS_OUTPUT} | xxd -r -p >$@
	truncate -s21 $@

${APPVER}: ${VERSION_DIGEST_STAMP} ${GLOBALDEPS}
	mkdir -p "$(dir $@)"
	echo ${GIT_DESC_APP_OUTPUT} | xxd -r -p >$@
	truncate -s21 $@
