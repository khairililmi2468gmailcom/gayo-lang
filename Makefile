CC = gcc
CFLAGS = -Wall -O2

.PHONY: all clean install test

all: gayoc

gayoc: gayoc.c
	$(CC) $(CFLAGS) gayoc.c -o gayoc

clean:
	rm -f gayoc *.o *.asm
	rm -f examples/*.o examples/*.asm
	find examples -type f -executable ! -name "*.gy" -delete 2>/dev/null || true

install: gayoc
	@echo "Menyalin 'gayo' dan 'gayoc' ke /usr/local/bin (butuh sudo)..."
	sudo cp gayo /usr/local/bin/gayo
	sudo cp gayoc /usr/local/bin/gayoc
	sudo chmod +x /usr/local/bin/gayo /usr/local/bin/gayoc
	@echo "Selesai. Coba: gayo examples/halo.gy"

test: gayoc
	@echo "Menjalankan semua contoh program di examples/ ..."
	@for f in examples/*.gy; do \
		echo "--- $$f ---"; \
		./gayo "$$f"; \
		echo ""; \
	done
