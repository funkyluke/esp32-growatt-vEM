CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
SRC = test_main.c ../../main/mb_proto.c ../../main/limiter.c

test: $(SRC)
	$(CC) $(CFLAGS) -I../../main $(SRC) -lm -o test_host
	./test_host

clean:
	rm -f test_host
