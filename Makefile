CC = gcc
SRC = main.c tinyosc.c globmatch.c
INC = tinyosc.h
BIN = bioscc

$(BIN): Makefile $(SRC) $(INC)
	$(CC) -Wall -Werror -O0 -g -o $(BIN) $(SRC)

clean: 
	rm -f $(BIN)

slup: Makefile slup.c cobs.c
	$(CC) -Wall -Werror slup.c cobs.c -o slup -I/opt/homebrew/Cellar/libserialport/0.1.2/include/ -L/opt/homebrew/Cellar/libserialport/0.1.2/lib -lserialport
