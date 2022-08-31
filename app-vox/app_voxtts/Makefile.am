pkgdir = $(asteriskmoduledir)
pkg_LTLIBRARIES = app_voxtts.la
PROTO_BUILT_SOURCES = \
	tts_api.pb.cc tts_api.pb.h \
	tts_api.grpc.pb.cc tts_api.grpc.pb.h
BUILT_SOURCES = ../thirdparty/inst/include/grpc/grpc.h   roots.pem.h $(PROTO_BUILT_SOURCES)

app_voxtts_la_SOURCES = \
	app_voxtts.c \
	stream_layers.c \
	bytequeue.cpp \
	channelbackend.cpp \
	channel.cpp \
	grpctts.cpp \
	grpctts_conf.c \
	job.cpp \
	$(PROTO_BUILT_SOURCES)

app_voxtts_la_CFLAGS = -Wall -pthread -O3 -Werror=implicit-function-declaration -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -fPIC -DAST_MODULE=\"app_voxtts\" \
	-DASTERISK_MODULE_VERSION_STRING=\"`git describe --tags --always`\" -fblocks
app_voxtts_la_CXXFLAGS = -Wall -pthread -O3 -std=c++11 -I../thirdparty/inst/include -fPIC -fblocks
app_voxtts_la_LDFLAGS = -Wl,-E -pthread -g -module -avoid-version -Wl,-fuse-ld=gold \
	../thirdparty/inst/lib/libprotobuf.a \
	../thirdparty/inst/lib/libprotoc.a \
	../thirdparty/inst/lib/libaddress_sorting.a \
	../thirdparty/inst/lib/libgpr.a \
	../thirdparty/inst/lib/libgrpc.a \
	../thirdparty/inst/lib/libgrpc++.a \
	../thirdparty/inst/lib/libgrpc_cronet.a \
	../thirdparty/inst/lib/libgrpc++_cronet.a \
	../thirdparty/inst/lib/libgrpc++_error_details.a \
	../thirdparty/inst/lib/libgrpc++_reflection.a \
	../thirdparty/inst/lib/libgrpc_unsecure.a \
	../thirdparty/inst/lib/libgrpc++_unsecure.a
app_playbackground_la_LIBTOOLFLAGS = --tag=disable-static

CLEANFILES=$(PROTO_BUILT_SOURCES) roots.pem.h


tts_api.pb.cc tts_api.pb.h: tts_api.proto
	../thirdparty/inst/bin/protoc -I . -I ../thirdparty/googleapis --cpp_out=. tts_api.proto

tts_api.grpc.pb.cc tts_api.grpc.pb.h: tts_api.proto
	../thirdparty/inst/bin/protoc -I . -I ../thirdparty/googleapis --grpc_out=. --plugin=protoc-gen-grpc=../thirdparty/inst/bin/grpc_cpp_plugin tts_api.proto


../thirdparty/inst/include/grpc/grpc.h:
	make -C ../thirdparty inst/include/grpc/grpc.h


roots.pem.h: ../thirdparty/grpc/etc/roots.pem
	echo "static unsigned char grpc_roots_pem[] = {" > roots.pem.h.tmp
	xxd -i ../thirdparty/grpc/etc/roots.pem | tail -n +2 | head -n -1 >> roots.pem.h.tmp
	mv roots.pem.h.tmp roots.pem.h


XMLDOC_FILES = app_voxtts.c

all-local: .xmldocs/app_voxtts-en_US.xml

.xmldocs/app_voxtts-en_US.xml_tmp: $(XMLDOC_FILES)
	mkdir -p .xmldocs
	@echo "Generating $@..."
	@echo "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" > $@
	@echo "<!DOCTYPE docs SYSTEM \"appdocsxml.dtd\">" >> $@
	@echo "<docs xmlns:xi=\"http://www.w3.org/2001/XInclude\">" >> $@
	@for i in $(XMLDOC_FILES); do \
		$(AWK) -f awk_get_documentation $$i >> $@ ; \
	done ;
	@echo "</docs>" >> $@
	@echo "Generating $@ done"

.xmldocs/app_voxtts-en_US.xml: $(XMLDOC_FILES)
	echo $@_tmp
	make $@_tmp || (rm -f $@_tmp; false)
	mv $@_tmp $@

install-data-local: .xmldocs/app_voxtts-en_US.xml
	test -d $(DESTDIR)$(asterisk_xmldoc_dir)/thirdparty || $(mkinstalldirs) $(DESTDIR)$(asterisk_xmldoc_dir)/thirdparty
	$(INSTALL) -m 644 .xmldocs/app_voxtts-en_US.xml $(DESTDIR)$(asterisk_xmldoc_dir)/thirdparty


clobber: distclean
	rm -rf ./{configure,aclocal.m4,libtool,ltmain.sh,autom4te.cache,config.h.in,Makefile.in,google,.xmldocs}
