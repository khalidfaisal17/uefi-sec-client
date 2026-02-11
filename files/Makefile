TARGET = uefi_sec_client
src = uefi_sec_client.c
obj = $(src:.c=.o)

# Headers
CFLAGS += -I$(RECIPE_SYSROOT)/usr/include/ion_headers

# Libraries - Must be defined here and linked at the end
LIBS = -lQseeComApi -ldmabufheap -lpthread

all: $(TARGET)

bin_PROGRAMS = uefi_sec_client

$(TARGET): $(obj)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(obj) $(TARGET)
