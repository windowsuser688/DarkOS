#include <elf.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *msg)
{
	if (errno)
		fprintf(stderr, "darkelf: %s: %s\n", msg, strerror(errno));
	else
		fprintf(stderr, "darkelf: %s\n", msg);
	exit(1);
}

static const char *etype(unsigned type)
{
	switch (type) {
	case ET_NONE: return "none";
	case ET_REL: return "relocatable";
	case ET_EXEC: return "executable";
	case ET_DYN: return "shared object";
	case ET_CORE: return "core";
	default: return "processor-specific/unknown";
	}
}

static const char *machine(unsigned machine)
{
	switch (machine) {
	case EM_386: return "i386";
	case EM_X86_64: return "x86-64";
	case EM_ARM: return "arm";
	case EM_AARCH64: return "aarch64";
	case EM_RISCV: return "riscv";
	default: return "unknown";
	}
}

static void read_at(FILE *fp, long off, void *buf, size_t len)
{
	if (fseek(fp, off, SEEK_SET) != 0)
		die("seek failed");
	if (fread(buf, 1, len, fp) != len)
		die("short read");
}

static void parse64(FILE *fp, const Elf64_Ehdr *eh)
{
	printf("class: ELF64\n");
	printf("type: %s (%u)\n", etype(eh->e_type), eh->e_type);
	printf("machine: %s (%u)\n", machine(eh->e_machine), eh->e_machine);
	printf("entry: 0x%" PRIx64 "\n", (uint64_t)eh->e_entry);
	printf("program_headers: %u at 0x%" PRIx64 "\n", eh->e_phnum, (uint64_t)eh->e_phoff);
	printf("section_headers: %u at 0x%" PRIx64 "\n", eh->e_shnum, (uint64_t)eh->e_shoff);

	if (eh->e_phoff && eh->e_phnum) {
		puts("\nprogram headers:");
		for (uint16_t i = 0; i < eh->e_phnum; i++) {
			Elf64_Phdr ph;
			read_at(fp, (long)(eh->e_phoff + (uint64_t)i * eh->e_phentsize), &ph, sizeof ph);
			printf("  [%u] type=%u off=0x%" PRIx64 " vaddr=0x%" PRIx64 " filesz=0x%" PRIx64 " memsz=0x%" PRIx64 " flags=0x%x\n",
			       i, ph.p_type, (uint64_t)ph.p_offset, (uint64_t)ph.p_vaddr,
			       (uint64_t)ph.p_filesz, (uint64_t)ph.p_memsz, ph.p_flags);
		}
	}

	if (eh->e_shoff && eh->e_shnum) {
		puts("\nsection headers:");
		for (uint16_t i = 0; i < eh->e_shnum; i++) {
			Elf64_Shdr sh;
			read_at(fp, (long)(eh->e_shoff + (uint64_t)i * eh->e_shentsize), &sh, sizeof sh);
			printf("  [%u] type=%u off=0x%" PRIx64 " size=0x%" PRIx64 " flags=0x%" PRIx64 "\n",
			       i, sh.sh_type, (uint64_t)sh.sh_offset, (uint64_t)sh.sh_size,
			       (uint64_t)sh.sh_flags);
		}
	}
}

static void parse32(FILE *fp, const Elf32_Ehdr *eh)
{
	printf("class: ELF32\n");
	printf("type: %s (%u)\n", etype(eh->e_type), eh->e_type);
	printf("machine: %s (%u)\n", machine(eh->e_machine), eh->e_machine);
	printf("entry: 0x%" PRIx32 "\n", eh->e_entry);
	printf("program_headers: %u at 0x%" PRIx32 "\n", eh->e_phnum, eh->e_phoff);
	printf("section_headers: %u at 0x%" PRIx32 "\n", eh->e_shnum, eh->e_shoff);

	if (eh->e_phoff && eh->e_phnum) {
		puts("\nprogram headers:");
		for (uint16_t i = 0; i < eh->e_phnum; i++) {
			Elf32_Phdr ph;
			read_at(fp, (long)(eh->e_phoff + (uint32_t)i * eh->e_phentsize), &ph, sizeof ph);
			printf("  [%u] type=%u off=0x%" PRIx32 " vaddr=0x%" PRIx32 " filesz=0x%" PRIx32 " memsz=0x%" PRIx32 " flags=0x%x\n",
			       i, ph.p_type, ph.p_offset, ph.p_vaddr, ph.p_filesz, ph.p_memsz, ph.p_flags);
		}
	}

	if (eh->e_shoff && eh->e_shnum) {
		puts("\nsection headers:");
		for (uint16_t i = 0; i < eh->e_shnum; i++) {
			Elf32_Shdr sh;
			read_at(fp, (long)(eh->e_shoff + (uint32_t)i * eh->e_shentsize), &sh, sizeof sh);
			printf("  [%u] type=%u off=0x%" PRIx32 " size=0x%" PRIx32 " flags=0x%" PRIx32 "\n",
			       i, sh.sh_type, sh.sh_offset, sh.sh_size, sh.sh_flags);
		}
	}
}

int main(int argc, char **argv)
{
	unsigned char ident[EI_NIDENT];
	FILE *fp;

	if (argc != 2) {
		fprintf(stderr, "usage: darkelf <elf-file>\n");
		return 2;
	}

	fp = fopen(argv[1], "rb");
	if (!fp)
		die(argv[1]);

	if (fread(ident, 1, sizeof ident, fp) != sizeof ident)
		die("not an ELF file");

	if (memcmp(ident, ELFMAG, SELFMAG) != 0)
		die("bad ELF magic");

	if (ident[EI_DATA] != ELFDATA2LSB)
		die("only little-endian ELF is supported");

	if (ident[EI_CLASS] == ELFCLASS64) {
		Elf64_Ehdr eh;
		read_at(fp, 0, &eh, sizeof eh);
		parse64(fp, &eh);
	} else if (ident[EI_CLASS] == ELFCLASS32) {
		Elf32_Ehdr eh;
		read_at(fp, 0, &eh, sizeof eh);
		parse32(fp, &eh);
	} else {
		die("unknown ELF class");
	}

	fclose(fp);
	return 0;
}

