objs 	:= start.o main.o
ld 	 	:= arm-linux-gnueabihf-ld
objcopy := arm-linux-gnueabihf-objcopy
objdump := arm-linux-gnueabihf-objdump
gcc		:= arm-linux-gnueabihf-gcc

ledc.bin:$(objs)
	$(ld) -Timx6ul.lds -o ledc.elf $^
	$(objcopy) -O binary -S ledc.elf $@
	$(objdump) -D -m arm ledc.elf > ledc.dis

%.o:%.s
	$(gcc) -Wall -nostdlib -c -O2 -o $@ $<

%.o:%.S 
	$(gcc) -Wall -nostdlib -c -O2 -o $@ $<

%.o:%.c
	$(gcc) -Wall -nostdlib -c -O2 -o $@ $<

clean:
	rm -rf *.o ledc.bin ledc.elf ledc.disS
