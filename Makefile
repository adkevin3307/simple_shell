all:
	flex scanner.l
	gcc -g lex.yy.c main.c -o main.out

clean:
	rm -rf lex.yy.c *.out

