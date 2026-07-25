CC = cc
CFLAGS = -O2 -Wall -Wextra
LDFLAGS = -framework AudioToolbox -framework CoreAudio -framework CoreFoundation

TARGET = audio_router

all: $(TARGET)

$(TARGET): audio_router.c
	$(CC) $(CFLAGS) -o $(TARGET) audio_router.c $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
