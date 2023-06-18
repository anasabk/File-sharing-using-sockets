OBJECTS = obj/client.o \
		  obj/server.o \
		  obj/common.o 
		#   obj/worker.o \
		#   obj/io_module.o \

SEARCH_PATH = include

GCCPARAMS = -Wall -g -std=gnu17 -lm -I$(SEARCH_PATH)

obj/%.o: src/%.c
	mkdir -p $(@D)
	gcc $(GCCPARAMS) -c $< -o $@

compile: $(OBJECTS)
	gcc $(GCCPARAMS) obj/common.o obj/server.o -o BibakBOXServer
	gcc $(GCCPARAMS) obj/common.o obj/client.o -o BibakBOXClient

clean:
	rm -rf obj BibakBOXClient BibakBOXServer test/clients/* test/server/*
