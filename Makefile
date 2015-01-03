
recolino: clean
	gcc -Wall -pedantic -O3 -o recolino recolino.c -ljack -lsndfile

clean:
	rm -f recolino

