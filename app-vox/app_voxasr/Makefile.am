pkgdir = $(asteriskmoduledir)
pkg_LTLIBRARIES = app_voxasr.la


PROTO_BUILT_SOURCES = asr_api.pb.cc asr_api.pb.h \
                      	asr_api.grpc.pb.cc asr_api.grpc.pb.h


BUILT_SOURCES = ../thirdparty/inst/include/grpc/grpc.h ../thirdparty/inst/lib/libjansson.a $(PROTO_BUILT_SOURCES)

app_voxasr_la_SOURCES = app_voxasr.c \
                         grpc_stt.cpp \
                         $(PROTO_BUILT_SOURCES)

app_voxasr_la_CFLAGS = -Wall -O3 -Werror=implicit-function-declaration -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -I../thirdparty/inst/include \
	-fPIC -DAST_MODULE=\"app_vox\" -DASTERISK_MODULE_VERSION_STRING=\"`git describe --tags --always`\"  -DDEBUG -g -fblocks
app_voxasr_la_CXXFLAGS = -Wall -O3 -std=c++11 -I../thirdparty/inst/include -fPIC -DDEBUG -g -fblocks
app_voxasr_la_CCFLAGS = -DDEBUG -g
app_voxasr_la_LDFLAGS = -Wl,-E -pthread -g -module -avoid-version  -Wl,-fuse-ld=gold \
                     	../thirdparty/inst/lib/libjansson.a \
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

app_voxasr_la_LIBTOOLFLAGS = --tag=disable-static
app_voxasr_la_LIBADD  = $(VOX_LIBS)



CLEANFILES=$(PROTO_BUILT_SOURCES)

asr_api.pb.cc asr_api.pb.h: asr_api.proto
	../thirdparty/inst/bin/protoc -I . -I ../thirdparty/googleapis -I ../thirdparty/inst/include --cpp_out=. asr_api.proto

asr_api.grpc.pb.cc asr_api.grpc.pb.h: asr_api.proto
	../thirdparty/inst/bin/protoc -I . -I ../thirdparty/googleapis -I ../thirdparty/inst/include --grpc_out=. --plugin=protoc-gen-grpc=../thirdparty/inst/bin/grpc_cpp_plugin asr_api.proto

../thirdparty/inst/include/grpc/grpc.h:
	make -C ../thirdparty inst/include/grpc/grpc.h

../thirdparty/inst/lib/libjansson.a:
	make -C ../thirdparty inst/lib/libjansson.a


XMLDOC_FILES = app_voxasr.c

all-local: .xmldocs/app_voxasr-en_US.xml

.xmldocs/app_voxasr-en_US.xml_tmp: $(XMLDOC_FILES)
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


.xmldocs/app_voxasr-en_US.xml: $(XMLDOC_FILES)
	echo $@_tmp
	make $@_tmp || (rm -f $@_tmp; false)
	mv $@_tmp $@

clean-local:
	rm -rf .xmldocs

install-data-local: .xmldocs/app_voxasr-en_US.xml
	test -d $(DESTDIR)$(asterisk_xmldoc_dir)/thirdparty || $(mkinstalldirs) $(DESTDIR)$(asterisk_xmldoc_dir)/thirdparty
	$(INSTALL) -m 644 .xmldocs/app_voxasr-en_US.xml $(DESTDIR)$(asterisk_xmldoc_dir)/thirdparty

load:
	asterisk -rx "module load app_voxasr.so"

unload:
	asterisk -rx "module unload app_voxasr.so"
