/* ============================================================
 *  GAYOC - Compiler untuk Bahasa Pemrograman Gayo
 *  ------------------------------------------------------------
 *  Ditulis dalam C. Mengompilasi file .gy menjadi assembly
 *  x86-64 (Linux, syscall langsung, tanpa libc), yang kemudian
 *  di-assemble (nasm) dan di-link (ld) menjadi biner ELF asli
 *  yang dijalankan CPU secara langsung.
 *
 *  Lihat README.md untuk dokumentasi lengkap bahasa Gayo.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#define UK_FILE   65536
#define UK_BUF    400000
#define MAKS_VAR  256
#define MAKS_LOKAL 128
#define MAKS_ARR  64
#define MAKS_PARAM 6
#define MAKS_LOOP 32

/* ------------------------------------------------------------
 * STATE GLOBAL COMPILER
 * ------------------------------------------------------------ */
char kode_sumber[UK_FILE];
int posisi = 0;

/* Empat "kanvas" tempat kita menulis potongan assembly.
 * Kita tulis ke kanvas yang berbeda tergantung konteks, lalu
 * SEMUA digabung di akhir dengan urutan yang benar (data, bss,
 * lalu text: _start dulu, baru definisi fungsi-fungsi). Ini
 * penting supaya definisi fungsi tidak "tertabrak jalan" oleh
 * alur eksekusi program utama, walau ditulis di tengah source. */
char buf_data[UK_BUF];  int len_data = 0;
char buf_bss[8192];     int len_bss = 0;
char buf_main[UK_BUF];  int len_main = 0;
char buf_funcs[UK_BUF]; int len_funcs = 0;

int butuh_cetak_bulet = 0;
int butuh_cetak_boolean = 0;
int label_counter = 0;
int dalam_fungsi = 0;

typedef struct {
    char nama[64];
    char tipe[16];        /* bulet, nyintak, boolean, mosop, arakoma, tong */
    char teks_nilai[256]; /* utk nyintak/mosop/arakoma: teks yg dicetak apa adanya */
    long elemen_tong[MAKS_ARR];
    int jumlah_elemen_tong;
} VariabelGlobal;
VariabelGlobal var_global[MAKS_VAR];
int jumlah_var_global = 0;

typedef struct {
    char nama[64];
    int offset; /* offset dari rbp, kelipatan 8, selalu negatif dipakainya */
} VariabelLokal;
VariabelLokal var_lokal[MAKS_LOKAL];
int jumlah_var_lokal = 0;

int loop_start_stack[MAKS_LOOP];
int loop_end_stack[MAKS_LOOP];
int loop_depth = 0;

const char *reg_param[MAKS_PARAM] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};

/* ------------------------------------------------------------
 * UTIL: emit ke kanvas
 * ------------------------------------------------------------ */
void emit_data(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    len_data += vsnprintf(buf_data + len_data, UK_BUF - len_data, fmt, a);
    va_end(a);
}
void emit_bss(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    len_bss += vsnprintf(buf_bss + len_bss, 8192 - len_bss, fmt, a);
    va_end(a);
}
/* emit() otomatis menulis ke buf_funcs kalau sedang di dalam body
 * fungsi (dalam_fungsi==1), atau ke buf_main kalau di kode utama.
 * Ini membuat semua fungsi parse_* tidak perlu tahu/peduli sedang
 * menulis ke kanvas yang mana. */
void emit(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    if (dalam_fungsi) len_funcs += vsnprintf(buf_funcs + len_funcs, UK_BUF - len_funcs, fmt, a);
    else              len_main  += vsnprintf(buf_main  + len_main,  UK_BUF - len_main,  fmt, a);
    va_end(a);
}
int label_baru() { return label_counter++; }

/* ------------------------------------------------------------
 * LEXER DASAR (beroperasi langsung di atas kode_sumber + posisi)
 * ------------------------------------------------------------ */
void lewati_spasi_dan_komentar() {
    while (kode_sumber[posisi] != '\0') {
        char c = kode_sumber[posisi];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') posisi++;
        else if (c == '!' && kode_sumber[posisi + 1] == '!') {
            while (kode_sumber[posisi] != '\n' && kode_sumber[posisi] != '\0') posisi++;
        } else break;
    }
}
int is_id_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

int cocokkan_kata(const char *kata) {
    int p = strlen(kata);
    if (strncmp(&kode_sumber[posisi], kata, p) != 0) return 0;
    if (is_id_char(kode_sumber[posisi + p])) return 0;
    posisi += p;
    return 1;
}
/* varian cocokkan_kata yang tidak mengubah 'posisi' kalau gagal maupun
 * berhasil -- dipakai untuk mengintip/lookahead tanpa berkomitmen. */
int intip_kata_di(int p, const char *kata) {
    int len = strlen(kata);
    if (strncmp(&kode_sumber[p], kata, len) != 0) return 0;
    if (p > 0 && is_id_char(kode_sumber[p - 1])) return 0;
    if (is_id_char(kode_sumber[p + len])) return 0;
    return 1;
}

int baca_identifier(char *hasil) {
    if (!isalpha((unsigned char)kode_sumber[posisi]) && kode_sumber[posisi] != '_') return -1;
    int n = 0;
    while (is_id_char(kode_sumber[posisi])) hasil[n++] = kode_sumber[posisi++];
    hasil[n] = '\0';
    return n;
}
int baca_teks_berkutip(char *hasil) {
    if (kode_sumber[posisi] != '"') return -1;
    posisi++;
    int n = 0;
    while (kode_sumber[posisi] != '"' && kode_sumber[posisi] != '\0') hasil[n++] = kode_sumber[posisi++];
    if (kode_sumber[posisi] != '"') return -1;
    posisi++;
    hasil[n] = '\0';
    return n;
}

int cari_var_lokal(const char *nama) {
    for (int i = 0; i < jumlah_var_lokal; i++)
        if (strcmp(var_lokal[i].nama, nama) == 0) return i;
    return -1;
}
int cari_var_global(const char *nama) {
    for (int i = 0; i < jumlah_var_global; i++)
        if (strcmp(var_global[i].nama, nama) == 0) return i;
    return -1;
}

/* ------------------------------------------------------------
 * PRA-PINDAI: hitung berapa banyak deklarasi lokal (ara/isen) di
 * dalam sebuah blok fungsi, supaya kita tahu ukuran stack frame
 * SEBELUM mulai menulis kode (dibutuhkan utk instruksi `sub rsp`
 * di awal fungsi). Tidak mengubah 'posisi' global sama sekali.
 * ------------------------------------------------------------ */
int hitung_lokal_di_blok(int mulai_setelah_kurung_buka) {
    int p = mulai_setelah_kurung_buka;
    int depth = 1;
    int jumlah = 0;
    while (kode_sumber[p] != '\0' && depth > 0) {
        if (kode_sumber[p] == '{') { depth++; p++; continue; }
        if (kode_sumber[p] == '}') { depth--; p++; continue; }
        if (intip_kata_di(p, "ara")) { jumlah++; p += 3; continue; }
        if (intip_kata_di(p, "isen")) { jumlah++; p += 4; continue; }
        p++;
    }
    return jumlah;
}

/* ------------------------------------------------------------
 * PARSER EKSPRESI (recursive descent, gaya "stack machine":
 * setiap parse_* di bawah, setelah selesai, meninggalkan HASIL
 * di PUNCAK runtime stack, lewat instruksi `push`. Operator biner
 * nanti tinggal `pop` dua nilai, olah, `push` lagi hasilnya.)
 *
 * Urutan prioritas (dari terendah ke tertinggi):
 *   or (ataupe) > and (urum) > not (nume) > persamaan (dis/gere dis)
 *   > perbandingan (lebih kul/kucak) > tambah/kurang > kali/bagi
 *   > minus unary > primary (angka, variabel, (...), panggil fungsi,
 *     akses tong[idx])
 * ------------------------------------------------------------ */
void parse_ekspresi(); /* deklarasi maju, karena saling rekursif */

void parse_panggilan_fungsi(const char *nama_fungsi) {
    /* posisi sekarang tepat setelah '(' */
    int jumlah_arg = 0;
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != ')') {
        while (1) {
            parse_ekspresi();  /* setiap argumen dievaluasi, hasil di-push */
            jumlah_arg++;
            lewati_spasi_dan_komentar();
            if (kode_sumber[posisi] == ',') { posisi++; lewati_spasi_dan_komentar(); continue; }
            break;
        }
    }
    if (kode_sumber[posisi] != ')') {
        fprintf(stderr, "Error: pemanggilan fungsi '%s' butuh ')' di posisi %d\n", nama_fungsi, posisi);
        exit(1);
    }
    posisi++;
    if (jumlah_arg > MAKS_PARAM) {
        fprintf(stderr, "Error: fungsi '%s' dipanggil dengan >%d argumen (maks %d)\n", nama_fungsi, MAKS_PARAM, MAKS_PARAM);
        exit(1);
    }
    /* pop argumen dari stack ke register, MUNDUR (argumen terakhir
     * yang di-push ada di puncak, dan itu argumen paling akhir) */
    for (int i = jumlah_arg - 1; i >= 0; i--) {
        emit("    pop %s\n", reg_param[i]);
    }
    emit("    call %s\n", nama_fungsi);
    /* hasil fungsi ada di rax (konvensi kita) -- caller boleh push
     * kalau ini dipakai sbg ekspresi; ditangani oleh pemanggil. */
}

void parse_primary() {
    lewati_spasi_dan_komentar();

    if (kode_sumber[posisi] == '(') {
        posisi++;
        parse_ekspresi();
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != ')') { fprintf(stderr, "Error: kurung ')' hilang di posisi %d\n", posisi); exit(1); }
        posisi++;
        return;
    }
    if (cocokkan_kata("nguk")) { emit("    mov rax, 1\n    push rax\n"); return; }
    if (cocokkan_kata("enggeh")) { emit("    mov rax, 0\n    push rax\n"); return; }
    if (kode_sumber[posisi] == '-' && isdigit((unsigned char)kode_sumber[posisi + 1])) {
        /* minus langsung menempel angka, ditangani juga di sini utk kesederhanaan */
    }
    if (isdigit((unsigned char)kode_sumber[posisi])) {
        long n = 0;
        while (isdigit((unsigned char)kode_sumber[posisi])) { n = n * 10 + (kode_sumber[posisi] - '0'); posisi++; }
        emit("    mov rax, %ld\n    push rax\n", n);
        return;
    }
    if (isalpha((unsigned char)kode_sumber[posisi]) || kode_sumber[posisi] == '_') {
        char nama[64];
        baca_identifier(nama);
        lewati_spasi_dan_komentar();

        if (kode_sumber[posisi] == '(') {
            posisi++;
            parse_panggilan_fungsi(nama);
            emit("    push rax\n");
            return;
        }
        if (kode_sumber[posisi] == '[') {
            posisi++;
            parse_ekspresi();  /* index, hasil di-push */
            lewati_spasi_dan_komentar();
            if (kode_sumber[posisi] != ']') { fprintf(stderr, "Error: ']' hilang setelah index array '%s'\n", nama); exit(1); }
            posisi++;
            int idxv = cari_var_global(nama);
            if (idxv == -1 || strcmp(var_global[idxv].tipe, "tong") != 0) {
                fprintf(stderr, "Error: '%s' bukan array (tong) yang dikenal\n", nama);
                exit(1);
            }
            emit("    pop rbx\n"); /* index */
            emit("    mov rax, [var_%s + rbx*8]\n", nama);
            emit("    push rax\n");
            return;
        }

        /* variabel biasa: cek lokal dulu, baru global */
        int li = dalam_fungsi ? cari_var_lokal(nama) : -1;
        if (li != -1) {
            emit("    mov rax, [rbp-%d]\n    push rax\n", var_lokal[li].offset);
            return;
        }
        int gi = cari_var_global(nama);
        if (gi != -1) {
            emit("    mov rax, [var_%s]\n    push rax\n", nama);
            return;
        }
        fprintf(stderr, "Error: variabel/fungsi '%s' tidak dikenal\n", nama);
        exit(1);
    }
    fprintf(stderr, "Error: ekspresi tidak valid di posisi %d ('%c')\n", posisi, kode_sumber[posisi]);
    exit(1);
}

void parse_unary_minus() {
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] == '-') {
        posisi++;
        parse_unary_minus();
        emit("    pop rax\n    neg rax\n    push rax\n");
        return;
    }
    parse_primary();
}

void parse_multiplicative() {
    parse_unary_minus();
    while (1) {
        lewati_spasi_dan_komentar();
        if (cocokkan_kata("kali")) {
            parse_unary_minus();
            emit("    pop rbx\n    pop rax\n    imul rax, rbx\n    push rax\n");
        } else if (cocokkan_kata("bagi")) {
            parse_unary_minus();
            emit("    pop rbx\n    pop rax\n    cqo\n    idiv rbx\n    push rax\n");
        } else break;
    }
}

void parse_additive() {
    parse_multiplicative();
    while (1) {
        lewati_spasi_dan_komentar();
        if (cocokkan_kata("tamah")) {
            parse_multiplicative();
            emit("    pop rbx\n    pop rax\n    add rax, rbx\n    push rax\n");
        } else if (cocokkan_kata("kurang")) {
            parse_multiplicative();
            emit("    pop rbx\n    pop rax\n    sub rax, rbx\n    push rax\n");
        } else break;
    }
}

/* helper: coba cocokkan urutan dua kata "kata1 kata2" dengan
 * backtrack -- kalau kata2 tidak ketemu, posisi dikembalikan ke
 * sebelum percobaan supaya tidak ikut termakan. */
int cocokkan_dua_kata(const char *k1, const char *k2) {
    int simpan = posisi;
    if (!cocokkan_kata(k1)) return 0;
    lewati_spasi_dan_komentar();
    if (!cocokkan_kata(k2)) { posisi = simpan; return 0; }
    return 1;
}

void parse_relational() {
    parse_additive();
    while (1) {
        lewati_spasi_dan_komentar();
        int simpan = posisi;
        if (cocokkan_kata("lebih")) {
            lewati_spasi_dan_komentar();
            if (cocokkan_kata("kul")) {
                lewati_spasi_dan_komentar();
                int sama_dengan = 0;
                int simpan2 = posisi;
                if (cocokkan_kata("dis")) {
                    lewati_spasi_dan_komentar();
                    if (cocokkan_kata("urum")) sama_dengan = 1;
                    else posisi = simpan2;
                }
                parse_additive();
                emit("    pop rbx\n    pop rax\n    cmp rax, rbx\n");
                emit(sama_dengan ? "    setge al\n" : "    setg al\n");
                emit("    movzx rax, al\n    push rax\n");
                continue;
            } else if (cocokkan_kata("kucak")) {
                lewati_spasi_dan_komentar();
                int sama_dengan = 0;
                int simpan2 = posisi;
                if (cocokkan_kata("dis")) {
                    lewati_spasi_dan_komentar();
                    if (cocokkan_kata("urum")) sama_dengan = 1;
                    else posisi = simpan2;
                }
                parse_additive();
                emit("    pop rbx\n    pop rax\n    cmp rax, rbx\n");
                emit(sama_dengan ? "    setle al\n" : "    setl al\n");
                emit("    movzx rax, al\n    push rax\n");
                continue;
            } else { posisi = simpan; break; }
        }
        break;
    }
}

void parse_equality() {
    parse_relational();
    while (1) {
        lewati_spasi_dan_komentar();
        int simpan = posisi;
        if (cocokkan_dua_kata("gere", "dis")) {
            parse_relational();
            emit("    pop rbx\n    pop rax\n    cmp rax, rbx\n    setne al\n    movzx rax, al\n    push rax\n");
            continue;
        }
        posisi = simpan;
        if (cocokkan_kata("dis")) {
            /* pastikan bukan awal dari "dis urum" milik operator relational
             * di atas (tidak mungkin nyasar ke sini karena sudah dikonsumsi
             * di parse_relational, tapi kita cek "urum" tidak ikut sini) */
            parse_relational();
            emit("    pop rbx\n    pop rax\n    cmp rax, rbx\n    sete al\n    movzx rax, al\n    push rax\n");
            continue;
        }
        break;
    }
}

void parse_not() {
    lewati_spasi_dan_komentar();
    if (cocokkan_kata("nume")) {
        parse_not();
        emit("    pop rax\n    xor rax, 1\n    push rax\n");
        return;
    }
    parse_equality();
}

void parse_and() {
    parse_not();
    while (1) {
        lewati_spasi_dan_komentar();
        if (cocokkan_kata("urum")) {
            parse_not();
            emit("    pop rbx\n    pop rax\n    and rax, rbx\n    push rax\n");
        } else break;
    }
}

void parse_or() {
    parse_and();
    while (1) {
        lewati_spasi_dan_komentar();
        if (cocokkan_kata("ataupe")) {
            parse_and();
            emit("    pop rbx\n    pop rax\n    or rax, rbx\n    push rax\n");
        } else break;
    }
}

void parse_ekspresi() { parse_or(); }

/* ------------------------------------------------------------
 * PARSER PERNYATAAN (statement)
 * ------------------------------------------------------------ */
void parse_blok();

void simpan_hasil_ke_variabel(const char *nama) {
    /* asumsi: hasil ekspresi sudah di stack (di-push), tinggal di-pop
     * dan disimpan ke variabel 'nama' (lokal kalau ada, else global) */
    int li = dalam_fungsi ? cari_var_lokal(nama) : -1;
    if (li != -1) {
        emit("    pop rax\n    mov [rbp-%d], rax\n", var_lokal[li].offset);
        return;
    }
    int gi = cari_var_global(nama);
    if (gi != -1) {
        if (strcmp(var_global[gi].tipe, "bulet") != 0 && strcmp(var_global[gi].tipe, "boolean") != 0) {
            fprintf(stderr, "Error: variabel '%s' bertipe %s, tidak bisa diisi hasil ekspresi\n", nama, var_global[gi].tipe);
            exit(1);
        }
        emit("    pop rax\n    mov [var_%s], rax\n", nama);
        return;
    }
    fprintf(stderr, "Error: variabel '%s' belum dideklarasi\n", nama);
    exit(1);
}

void deklarasi_variabel(int konstan) {
    (void)konstan; /* belum ditegakkan (belum ada cek larangan re-assign utk isen) */
    lewati_spasi_dan_komentar();
    char nama[64];
    if (baca_identifier(nama) < 0) { fprintf(stderr, "Error: nama variabel tidak valid\n"); exit(1); }
    if (dalam_fungsi ? (cari_var_lokal(nama) != -1) : (cari_var_global(nama) != -1)) {
        fprintf(stderr, "Error: variabel '%s' sudah dideklarasi\n", nama);
        exit(1);
    }
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != '=') { fprintf(stderr, "Error: '=' hilang setelah nama variabel '%s'\n", nama); exit(1); }
    posisi++;
    lewati_spasi_dan_komentar();

    if (kode_sumber[posisi] == '"') {
        char teks[256]; baca_teks_berkutip(teks);
        if (dalam_fungsi) { fprintf(stderr, "Error: variabel teks lokal ('%s') belum didukung di dalam fungsi\n", nama); exit(1); }
        VariabelGlobal v; memset(&v, 0, sizeof(v));
        strcpy(v.nama, nama); strcpy(v.tipe, "nyintak"); strcpy(v.teks_nilai, teks);
        var_global[jumlah_var_global++] = v;
    }
    else if (cocokkan_kata("mosop")) {
        if (dalam_fungsi) { fprintf(stderr, "Error: 'mosop' lokal belum didukung di dalam fungsi\n"); exit(1); }
        VariabelGlobal v; memset(&v, 0, sizeof(v));
        strcpy(v.nama, nama); strcpy(v.tipe, "mosop"); strcpy(v.teks_nilai, "mosop");
        var_global[jumlah_var_global++] = v;
    }
    else if (cocokkan_kata("tong")) {
        if (dalam_fungsi) { fprintf(stderr, "Error: array (tong) lokal belum didukung di dalam fungsi\n"); exit(1); }
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != '[') { fprintf(stderr, "Error: 'tong' harus diikuti '['\n"); exit(1); }
        posisi++;
        VariabelGlobal v; memset(&v, 0, sizeof(v));
        strcpy(v.nama, nama); strcpy(v.tipe, "tong");
        lewati_spasi_dan_komentar();
        while (kode_sumber[posisi] != ']') {
            int neg = 0;
            if (kode_sumber[posisi] == '-') { neg = 1; posisi++; }
            long n = 0;
            while (isdigit((unsigned char)kode_sumber[posisi])) { n = n * 10 + (kode_sumber[posisi]-'0'); posisi++; }
            v.elemen_tong[v.jumlah_elemen_tong++] = neg ? -n : n;
            lewati_spasi_dan_komentar();
            if (kode_sumber[posisi] == ',') { posisi++; lewati_spasi_dan_komentar(); }
        }
        posisi++; /* lewati ']' */
        var_global[jumlah_var_global++] = v;
    }
    else if (isdigit((unsigned char)kode_sumber[posisi]) && strchr(&kode_sumber[posisi], '.') != NULL &&
             ({ int q = posisi; while (isdigit((unsigned char)kode_sumber[q])) q++; kode_sumber[q] == '.'; })) {
        /* arakoma: hanya dukungan cetak apa adanya (belum ada operasi pecahan) */
        int awal = posisi;
        while (isdigit((unsigned char)kode_sumber[posisi])) posisi++;
        posisi++; /* titik */
        while (isdigit((unsigned char)kode_sumber[posisi])) posisi++;
        int panjang = posisi - awal;
        char teks[64]; strncpy(teks, &kode_sumber[awal], panjang); teks[panjang] = '\0';
        if (dalam_fungsi) { fprintf(stderr, "Error: variabel arakoma lokal belum didukung\n"); exit(1); }
        VariabelGlobal v; memset(&v, 0, sizeof(v));
        strcpy(v.nama, nama); strcpy(v.tipe, "arakoma"); strcpy(v.teks_nilai, teks);
        var_global[jumlah_var_global++] = v;
    }
    else {
        /* --- CATATAN NILAI BOOLEAN LITERAL LANGSUNG (nguk/enggeh) --- */
        int simpan = posisi;
        int lit_bool = -1;
        if (cocokkan_kata("nguk")) lit_bool = 1;
        else if (cocokkan_kata("enggeh")) lit_bool = 0;

        if (lit_bool != -1) {
            if (dalam_fungsi) {
                var_lokal[jumlah_var_lokal].offset = (jumlah_var_lokal + 1) * 8;
                strcpy(var_lokal[jumlah_var_lokal].nama, nama);
                emit("    mov qword [rbp-%d], %d\n", var_lokal[jumlah_var_lokal].offset, lit_bool);
                jumlah_var_lokal++;
            } else {
                VariabelGlobal v; memset(&v, 0, sizeof(v));
                strcpy(v.nama, nama); strcpy(v.tipe, "boolean");
                var_global[jumlah_var_global++] = v;
                emit_data("    var_%s: dq %d\n", nama, lit_bool);
            }
        } else {
            posisi = simpan;
            /* --- UMUM: ekspresi angka/boolean hasil hitungan --- */
            parse_ekspresi();
            if (dalam_fungsi) {
                var_lokal[jumlah_var_lokal].offset = (jumlah_var_lokal + 1) * 8;
                strcpy(var_lokal[jumlah_var_lokal].nama, nama);
                emit("    pop rax\n    mov [rbp-%d], rax\n", var_lokal[jumlah_var_lokal].offset);
                jumlah_var_lokal++;
            } else {
                VariabelGlobal v; memset(&v, 0, sizeof(v));
                strcpy(v.nama, nama); strcpy(v.tipe, "bulet");
                var_global[jumlah_var_global++] = v;
                emit_data("    var_%s: dq 0\n", nama);
                emit("    pop rax\n    mov [var_%s], rax\n", nama);
            }
            butuh_cetak_bulet = 1;
        }
    }

    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah deklarasi '%s'\n", nama); exit(1); }
    posisi++;
}

void parse_turuhen() {
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] == '"') {
        char teks[256]; baca_teks_berkutip(teks);
        int id = label_baru();
        emit_data("    pesan%d: db \"%s\", 10\n    panjang_pesan%d equ $ - pesan%d\n", id, teks, id, id);
        emit("    mov rax, 1\n    mov rdi, 1\n    mov rsi, pesan%d\n    mov rdx, panjang_pesan%d\n    syscall\n", id, id);
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah turuhen\n"); exit(1); }
        posisi++;
        return;
    }

    /* coba jalur cepat: variabel tunggal langsung diikuti ';' -> cetak
     * sesuai tipe aslinya (lebih presisi drpd selalu jadi ekspresi umum) */
    int simpan = posisi;
    if (isalpha((unsigned char)kode_sumber[posisi]) || kode_sumber[posisi] == '_') {
        char nama[64];
        baca_identifier(nama);
        int posisi_setelah_nama = posisi;
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] == ';') {
            /* ya, ini variabel tunggal berdiri sendiri */
            int gi = cari_var_global(nama);
            int li = dalam_fungsi ? cari_var_lokal(nama) : -1;
            if (li != -1) {
                emit("    mov rax, [rbp-%d]\n", var_lokal[li].offset);
                emit("    call cetak_bulet\n");
                butuh_cetak_bulet = 1;
            } else if (gi != -1) {
                VariabelGlobal *v = &var_global[gi];
                if (strcmp(v->tipe, "bulet") == 0) {
                    emit("    mov rax, [var_%s]\n    call cetak_bulet\n", nama);
                    butuh_cetak_bulet = 1;
                } else if (strcmp(v->tipe, "boolean") == 0) {
                    emit("    mov rax, [var_%s]\n    call cetak_boolean\n", nama);
                    butuh_cetak_boolean = 1;
                } else {
                    /* nyintak, mosop, arakoma: teks statis */
                    emit_data("    var_%s_teks: db \"%s\", 10\n    panjang_var_%s_teks equ $ - var_%s_teks\n",
                              nama, v->teks_nilai, nama, nama);
                    emit("    mov rax, 1\n    mov rdi, 1\n    mov rsi, var_%s_teks\n    mov rdx, panjang_var_%s_teks\n    syscall\n",
                         nama, nama);
                }
            } else {
                fprintf(stderr, "Error: variabel '%s' belum dideklarasi\n", nama);
                exit(1);
            }
            posisi++; /* lewati ';' */
            return;
        }
        (void)posisi_setelah_nama;
    }
    /* bukan variabel tunggal -> mundurkan posisi, proses sebagai ekspresi umum */
    posisi = simpan;
    parse_ekspresi();
    emit("    pop rax\n    call cetak_bulet\n");
    butuh_cetak_bulet = 1;
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah turuhen\n"); exit(1); }
    posisi++;
}

void parse_ike() {
    /* posisi sekarang tepat setelah kata "ike" pertama sudah dikonsumsi caller */
    int id_akhir = label_baru();
    int label_akhir = id_akhir;
    int lanjut_chain = 1;

    while (lanjut_chain) {
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != '(') { fprintf(stderr, "Error: 'ike'/'ike gere' butuh '(' \n"); exit(1); }
        posisi++;
        parse_ekspresi();
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != ')') { fprintf(stderr, "Error: ')' hilang pada kondisi ike\n"); exit(1); }
        posisi++;
        int label_next = label_baru();
        emit("    pop rax\n    cmp rax, 0\n    je etiket%d\n", label_next);
        parse_blok();
        emit("    jmp etiket%d\n", label_akhir);
        emit("etiket%d:\n", label_next);

        /* cek lanjutan: "ike gere (...)" atau "kegere { ... }" atau selesai */
        lewati_spasi_dan_komentar();
        int simpan = posisi;
        if (cocokkan_kata("ike")) {
            lewati_spasi_dan_komentar();
            if (cocokkan_kata("gere")) {
                continue; /* lanjut loop: parse kondisi berikutnya */
            }
            posisi = simpan; /* bukan "ike gere", itu statement baru yg terpisah */
            lanjut_chain = 0;
        } else if (cocokkan_kata("kegere")) {
            parse_blok();
            lanjut_chain = 0;
        } else {
            posisi = simpan;
            lanjut_chain = 0;
        }
    }
    emit("etiket%d:\n", label_akhir);
}

void parse_iwan() {
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != '(') { fprintf(stderr, "Error: 'iwan' butuh '('\n"); exit(1); }
    posisi++;
    int label_mulai = label_baru();
    int label_akhir = label_baru();
    emit("etiket%d:\n", label_mulai);
    parse_ekspresi();
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != ')') { fprintf(stderr, "Error: ')' hilang pada kondisi iwan\n"); exit(1); }
    posisi++;
    emit("    pop rax\n    cmp rax, 0\n    je etiket%d\n", label_akhir);

    loop_start_stack[loop_depth] = label_mulai;
    loop_end_stack[loop_depth] = label_akhir;
    loop_depth++;
    parse_blok();
    loop_depth--;

    emit("    jmp etiket%d\n", label_mulai);
    emit("etiket%d:\n", label_akhir);
}

/* deklarasi maju */
void parse_pernyataan();

void parse_kin() {
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != '(') { fprintf(stderr, "Error: 'kin' butuh '('\n"); exit(1); }
    posisi++;
    lewati_spasi_dan_komentar();

    parse_pernyataan(); /* inisialisasi, contoh: ara i = 0;  (sudah termasuk ';') */

    int label_cond = label_baru();
    int label_step = label_baru();
    int label_akhir = label_baru();

    emit("etiket%d:\n", label_cond);
    parse_ekspresi();
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah kondisi 'kin'\n"); exit(1); }
    posisi++;
    emit("    pop rax\n    cmp rax, 0\n    je etiket%d\n", label_akhir);

    /* --- langkah (step) HARUS diparse teksnya sekarang, tapi kodenya
     * baru dieksekusi setelah body -- jadi kita simpan teks sumbernya
     * dan proses ulang setelah body di-emit. Untuk kesederhanaan, kita
     * catat posisi awal & akhir teks langkah lalu emit ulang dgn
     * menjalankan parser di posisi tsb setelah body selesai. */
    int mulai_langkah = posisi;
    /* lewati teks langkah tanpa emit (mode "hitung saja") dengan cara
     * sederhana: cari posisi ')' yang berpasangan (tidak ada nested
     * kurung yg valid dlm langkah sederhana kita, jadi cari ')' pertama
     * pada depth 0) */
    int depth = 0;
    while (!(kode_sumber[posisi] == ')' && depth == 0)) {
        if (kode_sumber[posisi] == '(') depth++;
        if (kode_sumber[posisi] == ')') depth--;
        posisi++;
    }
    int akhir_langkah = posisi;
    posisi++; /* lewati ')' */

    loop_start_stack[loop_depth] = label_step;
    loop_end_stack[loop_depth] = label_akhir;
    loop_depth++;
    parse_blok();
    loop_depth--;

    emit("etiket%d:\n", label_step);
    /* proses ulang teks langkah SEKARANG, emit kodenya di sini */
    int simpan_posisi_global = posisi;
    posisi = mulai_langkah;
    char nama[64];
    lewati_spasi_dan_komentar();
    baca_identifier(nama);
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != '=') { fprintf(stderr, "Error: langkah 'kin' harus berupa 'variabel = ekspresi'\n"); exit(1); }
    posisi++;
    parse_ekspresi();
    simpan_hasil_ke_variabel(nama);
    posisi = akhir_langkah + 1 > simpan_posisi_global ? simpan_posisi_global : simpan_posisi_global;
    /* (posisi sudah benar krn kita set balik ke simpan_posisi_global) */
    posisi = simpan_posisi_global;

    emit("    jmp etiket%d\n", label_cond);
    emit("etiket%d:\n", label_akhir);
}

void parse_pernyataan() {
    lewati_spasi_dan_komentar();

    if (cocokkan_kata("isen")) { deklarasi_variabel(1); return; }
    if (cocokkan_kata("ara"))  { deklarasi_variabel(0); return; }
    if (cocokkan_kata("turuhen")) { parse_turuhen(); return; }
    if (cocokkan_kata("ike")) { parse_ike(); return; }
    if (cocokkan_kata("iwan")) { parse_iwan(); return; }
    if (cocokkan_kata("kin")) { parse_kin(); return; }
    if (cocokkan_kata("teduh")) {
        if (loop_depth == 0) { fprintf(stderr, "Error: 'teduh' dipakai di luar loop\n"); exit(1); }
        emit("    jmp etiket%d\n", loop_end_stack[loop_depth - 1]);
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah teduh\n"); exit(1); }
        posisi++;
        return;
    }
    if (cocokkan_kata("lanyut")) {
        if (loop_depth == 0) { fprintf(stderr, "Error: 'lanyut' dipakai di luar loop\n"); exit(1); }
        emit("    jmp etiket%d\n", loop_start_stack[loop_depth - 1]);
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah lanyut\n"); exit(1); }
        posisi++;
        return;
    }
    if (cocokkan_kata("ulaken")) {
        if (!dalam_fungsi) { fprintf(stderr, "Error: 'ulaken' hanya boleh di dalam fungsi\n"); exit(1); }
        parse_ekspresi();
        emit("    pop rax\n    mov rsp, rbp\n    pop rbp\n    ret\n");
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah ulaken\n"); exit(1); }
        posisi++;
        return;
    }
    if (cocokkan_kata("talun")) {
        lewati_spasi_dan_komentar();
        char nama[64];
        if (baca_identifier(nama) < 0) { fprintf(stderr, "Error: 'talun' harus diikuti nama fungsi\n"); exit(1); }
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != '(') { fprintf(stderr, "Error: 'talun %s' butuh '('\n", nama); exit(1); }
        posisi++;
        parse_panggilan_fungsi(nama);
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah talun\n"); exit(1); }
        posisi++;
        return;
    }

    /* identifier di awal: bisa jadi assignment biasa "nama = expr;"
     * atau assignment elemen array "nama[idx] = expr;" */
    if (isalpha((unsigned char)kode_sumber[posisi]) || kode_sumber[posisi] == '_') {
        char nama[64];
        baca_identifier(nama);
        lewati_spasi_dan_komentar();
        if (kode_sumber[posisi] == '[') {
            posisi++;
            parse_ekspresi(); /* index -> di stack */
            lewati_spasi_dan_komentar();
            if (kode_sumber[posisi] != ']') { fprintf(stderr, "Error: ']' hilang\n"); exit(1); }
            posisi++;
            lewati_spasi_dan_komentar();
            if (kode_sumber[posisi] != '=') { fprintf(stderr, "Error: '=' diharapkan setelah 'nama[idx]'\n"); exit(1); }
            posisi++;
            parse_ekspresi(); /* nilai baru -> di stack (di atas index) */
            lewati_spasi_dan_komentar();
            if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang\n"); exit(1); }
            posisi++;
            int gi = cari_var_global(nama);
            if (gi == -1 || strcmp(var_global[gi].tipe, "tong") != 0) {
                fprintf(stderr, "Error: '%s' bukan array (tong)\n", nama); exit(1);
            }
            emit("    pop rax\n    pop rbx\n    mov [var_%s + rbx*8], rax\n", nama);
            return;
        }
        if (kode_sumber[posisi] == '=') {
            posisi++;
            parse_ekspresi();
            lewati_spasi_dan_komentar();
            if (kode_sumber[posisi] != ';') { fprintf(stderr, "Error: ';' hilang setelah assignment '%s'\n", nama); exit(1); }
            posisi++;
            simpan_hasil_ke_variabel(nama);
            return;
        }
        fprintf(stderr, "Error: pernyataan tidak dikenal setelah '%s' di posisi %d\n", nama, posisi);
        exit(1);
    }

    fprintf(stderr, "Error: pernyataan tidak dikenal di posisi %d ('%c')\n", posisi, kode_sumber[posisi]);
    exit(1);
}

void parse_blok() {
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != '{') { fprintf(stderr, "Error: '{' diharapkan di posisi %d\n", posisi); exit(1); }
    posisi++;
    lewati_spasi_dan_komentar();
    while (kode_sumber[posisi] != '}' && kode_sumber[posisi] != '\0') {
        parse_pernyataan();
        lewati_spasi_dan_komentar();
    }
    if (kode_sumber[posisi] != '}') { fprintf(stderr, "Error: '}' hilang (blok tidak tertutup)\n"); exit(1); }
    posisi++;
}

void parse_kinsa() {
    lewati_spasi_dan_komentar();
    char nama_fungsi[64];
    if (baca_identifier(nama_fungsi) < 0) { fprintf(stderr, "Error: nama fungsi tidak valid\n"); exit(1); }
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != '(') { fprintf(stderr, "Error: fungsi '%s' butuh '('\n", nama_fungsi); exit(1); }
    posisi++;

    char nama_param[MAKS_PARAM][64];
    int jumlah_param = 0;
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != ')') {
        while (1) {
            lewati_spasi_dan_komentar();
            if (baca_identifier(nama_param[jumlah_param]) < 0) { fprintf(stderr, "Error: nama parameter tidak valid\n"); exit(1); }
            jumlah_param++;
            lewati_spasi_dan_komentar();
            if (kode_sumber[posisi] == ',') { posisi++; continue; }
            break;
        }
    }
    if (kode_sumber[posisi] != ')') { fprintf(stderr, "Error: ')' hilang pada deklarasi fungsi '%s'\n", nama_fungsi); exit(1); }
    posisi++;
    lewati_spasi_dan_komentar();
    if (kode_sumber[posisi] != '{') { fprintf(stderr, "Error: '{' hilang pada body fungsi '%s'\n", nama_fungsi); exit(1); }

    int jumlah_lokal_dideklarasi = hitung_lokal_di_blok(posisi + 1);
    int total_slot = jumlah_param + jumlah_lokal_dideklarasi;

    jumlah_var_lokal = 0;
    for (int i = 0; i < jumlah_param; i++) {
        strcpy(var_lokal[i].nama, nama_param[i]);
        var_lokal[i].offset = (i + 1) * 8;
    }
    jumlah_var_lokal = jumlah_param;

    dalam_fungsi = 1;
    emit("\n%s:\n", nama_fungsi);
    emit("    push rbp\n    mov rbp, rsp\n    sub rsp, %d\n", total_slot > 0 ? total_slot * 8 : 8);
    for (int i = 0; i < jumlah_param; i++) {
        emit("    mov [rbp-%d], %s\n", (i + 1) * 8, reg_param[i]);
    }

    parse_blok();

    /* fallback kalau tidak ada 'ulaken' eksplisit sampai akhir fungsi */
    emit("    mov rax, 0\n    mov rsp, rbp\n    pop rbp\n    ret\n");
    dalam_fungsi = 0;
}

/* ------------------------------------------------------------
 * MAIN
 * ------------------------------------------------------------ */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Cara pakai: %s input.gy output.asm\n", argv[0]);
        return 1;
    }
    FILE *fin = fopen(argv[1], "r");
    if (!fin) { fprintf(stderr, "Error: tidak bisa membuka %s\n", argv[1]); return 1; }
    int n = fread(kode_sumber, 1, UK_FILE - 1, fin);
    kode_sumber[n] = '\0';
    fclose(fin);

    lewati_spasi_dan_komentar();
    while (kode_sumber[posisi] != '\0') {
        if (cocokkan_kata("kinsa")) parse_kinsa();
        else parse_pernyataan();
        lewati_spasi_dan_komentar();
    }

    /* exit(0) di akhir kode utama */
    emit("    mov rax, 60\n    mov rdi, 0\n    syscall\n");

    /* rutin bawaan */
    char buf_rutin[8192]; int len_rutin = 0;
    if (butuh_cetak_bulet) {
        emit_bss("    buffer_angka resb 24\n");
        len_rutin += snprintf(buf_rutin + len_rutin, sizeof(buf_rutin) - len_rutin,
            "\ncetak_bulet:\n"
            "    ; Input: nilai integer di RAX. Cetak sbg teks desimal + newline.\n"
            "    mov r10, rax\n"
            "    lea rcx, [buffer_angka + 23]\n"
            "    mov byte [rcx], 10\n"
            "    mov r8, 0\n"
            "    mov rax, r10\n"
            "    cmp rax, 0\n"
            "    jge .tidak_negatif\n"
            "    mov r8, 1\n"
            "    neg rax\n"
            ".tidak_negatif:\n"
            "    cmp rax, 0\n"
            "    jne .loop\n"
            "    dec rcx\n"
            "    mov byte [rcx], '0'\n"
            "    jmp .selesai\n"
            ".loop:\n"
            "    cmp rax, 0\n"
            "    je .cek_negatif\n"
            "    dec rcx\n"
            "    xor rdx, rdx\n"
            "    mov rbx, 10\n"
            "    div rbx\n"
            "    add rdx, '0'\n"
            "    mov [rcx], dl\n"
            "    jmp .loop\n"
            ".cek_negatif:\n"
            "    cmp r8, 1\n"
            "    jne .selesai\n"
            "    dec rcx\n"
            "    mov byte [rcx], '-'\n"
            ".selesai:\n"
            "    mov rsi, rcx\n"
            "    lea rdx, [buffer_angka + 24]\n"
            "    sub rdx, rcx\n"
            "    mov rax, 1\n"
            "    mov rdi, 1\n"
            "    syscall\n"
            "    ret\n");
    }
    if (butuh_cetak_boolean) {
        emit_data("    teks_nguk: db \"nguk\", 10\n");
        emit_data("    teks_enggeh: db \"enggeh\", 10\n");
        len_rutin += snprintf(buf_rutin + len_rutin, sizeof(buf_rutin) - len_rutin,
            "\ncetak_boolean:\n"
            "    ; Input: 0 atau 1 di RAX. Cetak \"enggeh\"/\"nguk\".\n"
            "    cmp rax, 0\n"
            "    je .false\n"
            "    mov rsi, teks_nguk\n"
            "    mov rdx, 5\n"
            "    jmp .cetak\n"
            ".false:\n"
            "    mov rsi, teks_enggeh\n"
            "    mov rdx, 7\n"
            ".cetak:\n"
            "    mov rax, 1\n"
            "    mov rdi, 1\n"
            "    syscall\n"
            "    ret\n");
    }

    /* array (tong) global -> data */
    for (int i = 0; i < jumlah_var_global; i++) {
        if (strcmp(var_global[i].tipe, "tong") == 0) {
            emit_data("    var_%s: dq ", var_global[i].nama);
            for (int j = 0; j < var_global[i].jumlah_elemen_tong; j++) {
                emit_data("%ld%s", var_global[i].elemen_tong[j], (j == var_global[i].jumlah_elemen_tong - 1) ? "\n" : ", ");
            }
        }
    }

    FILE *out = fopen(argv[2], "w");
    if (!out) { fprintf(stderr, "Error: tidak bisa membuat %s\n", argv[2]); return 1; }
    fprintf(out, "; Di-generate OTOMATIS oleh gayoc (compiler bahasa Gayo)\n\n");
    fprintf(out, "section .data\n%s\n", buf_data);
    if (len_bss > 0) fprintf(out, "section .bss\n%s\n", buf_bss);
    fprintf(out, "section .text\n    global _start\n\n_start:\n%s\n%s\n%s\n", buf_main, buf_funcs, buf_rutin);
    fclose(out);

    printf("Kompilasi berhasil: %s -> %s\n", argv[1], argv[2]);
    return 0;
}
