# GrsProbe - Windows CE ARM cross-compile Makefile
#
# Derleyici: arm-mingw32ce (mingw32ce toolchain)
# Kurulum:   http://cegcc.sourceforge.net/
#
# Kullanım:
#   make          -> GrsProbe.exe üretir (ARM WinCE)
#   make clean    -> temizle
#
# NOT: Visual Studio ile derlemek için GrsProbe.vcproj kullanın
# Platform: Windows CE 5.0 / 6.0, ARMV4I

# ---- Toolchain ----
CC  = arm-mingw32ce-g++
RC  = arm-mingw32ce-windres

# ---- Flags ----
CFLAGS = \
    -DUNICODE \
    -D_UNICODE \
    -D_WIN32_WCE=0x0500 \
    -DUNDER_CE=0x0500 \
    -DWIN32 \
    -DARM \
    -O1 \
    -Wall \
    -fno-exceptions \
    -fno-rtti

# Windows CE için gerekli kütüphaneler
# coredll = ana CE kütüphanesi (kernel32+user32+gdi32 yerine)
LIBS = -lcoredll -lcommctrl -lole32

LDFLAGS = \
    -Wl,--subsystem,windows \
    -mwindows

# ---- Hedefler ----
TARGET = GrsProbe.exe
SRCS   = GrsProbe.cpp
OBJS   = GrsProbe.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Derlendi: $(TARGET)"
	@ls -la $(TARGET)

$(OBJS): $(SRCS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	del $(OBJS) $(TARGET) 2>nul || rm -f $(OBJS) $(TARGET)

.PHONY: all clean
