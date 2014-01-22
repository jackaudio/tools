MAINTAINERCLEANFILES    = Makefile.in

#
# in-process ALSA/JACK MIDI bridge client
#

zalsadir = $(ADDON_DIR)

zalsa_LTLIBRARIES = zalsa_in.la zalsa_out.la

zalsa_in_la_CPPFLAGS = -DAPPNAME=\"zalsa_in\" -DVERSION=\"0.4.0\"
zalsa_in_la_SOURCES = zita-a2j.cc alsathread.cc jackclient.cc pxthread.cc lfqueue.cc
zalsa_in_la_LDFLAGS = -module -avoid-version -lzita-resampler -lzita-alsa-pcmi -lasound -lpthread -lm -lrt @OS_LDFLAGS@
zalsa_in_la_LDFLAGS += -L$(top_builddir)/libjack -ljack

zalsa_out_la_CPPFLAGS = -DAPPNAME=\"zalsa_out\" -DVERSION=\"0.4.0\"
zalsa_out_la_SOURCES = zita-j2a.cc alsathread.cc jackclient.cc pxthread.cc lfqueue.cc
zalsa_out_la_LDFLAGS = -module -avoid-version -lzita-resampler -lzita-alsa-pcmi -lasound -lpthread -lm -lrt @OS_LDFLAGS@
zalsa_out_la_LDFLAGS += -L$(top_builddir)/libjack -ljack

noinst_HEADERS = alsathread.h jackclient.h lfqueue.h pxthread.h 

