/* libunwind - a platform-independent unwind library

This file is part of libunwind.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.  */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

/* Endian detection */
#include <limits.h>
#if defined(HAVE_BYTESWAP_H)
#include <byteswap.h>
#endif

#if defined(HAVE_ELF_H)
# include <elf.h>
#elif defined(HAVE_SYS_ELF_H)
# include <sys/elf.h>
#endif
#include <sys/procfs.h> /* struct elf_prstatus */

#include "_UCD_lib.h"
#include "_UCD_internal.h"


struct UCD_info *
_UCD_create(const char *filename)
{
  union
    {
      Elf32_Ehdr h32;
      Elf64_Ehdr h64;
    } elf_header;
#define elf_header32 elf_header.h32
#define elf_header64 elf_header.h64
  bool _64bits;

  struct UCD_info *ui = memset(malloc(sizeof(*ui)), 0, sizeof(*ui));
  ui->edi.di_cache.format = -1;
  ui->edi.di_debug.format = -1;
#if UNW_TARGET_IA64
  ui->edi.ktab.format = -1;
#endif

  int fd = ui->coredump_fd = open(filename, O_RDONLY);
  if (fd < 0)
    goto err;
  ui->coredump_filename = strdup(filename);

  /* No sane ELF32 file is going to be smaller then ELF64 _header_,
   * so let's just read 64-bit sized one.
   */
  if (read(fd, &elf_header64, sizeof(elf_header64)) != sizeof(elf_header64))
    {
      Debug(0, "'%s' is not an ELF file\n", filename);
      goto err;
    }

  if (memcmp(&elf_header32, ELFMAG, SELFMAG) != 0)
    {
      Debug(0, "'%s' is not an ELF file\n", filename);
      goto err;
    }

  if (elf_header32.e_ident[EI_CLASS] != ELFCLASS32
   && elf_header32.e_ident[EI_CLASS] != ELFCLASS64)
    {
      Debug(0, "'%s' is not a 32/64 bit ELF file\n", filename);
      goto err;
    }

  if (target_is_big_endian() && (elf_header32.e_ident[EI_DATA] == ELFDATA2LSB))
    {
      Debug(0, "'%s' is endian-incompatible\n", filename);
      goto err;
    }

  _64bits = (elf_header32.e_ident[EI_CLASS] == ELFCLASS64);
  if (_64bits && sizeof(elf_header64.e_entry) > sizeof(off_t))
    {
      Debug(0, "Can't process '%s': 64-bit file "
               "while only %ld bits are supported",
            filename, 8L * sizeof(off_t));
      goto err;
    }

  /* paranoia check */
  if (_64bits
            ? 0 /* todo: (elf_header64.e_ehsize != NN || elf_header64.e_phentsize != NN) */
            : (elf_header32.e_ehsize != 52 || elf_header32.e_phentsize != 32)
  )
    {
      Debug(0, "'%s' has wrong e_ehsize or e_phentsize\n", filename);
      goto err;
    }

  off_t ofs = (_64bits ? elf_header64.e_phoff : elf_header32.e_phoff);
  if (lseek(fd, ofs, SEEK_SET) != ofs)
    {
      Debug(0, "Can't read phdrs from '%s'\n", filename);
      goto err;
    }
  unsigned size = ui->phdrs_count = (_64bits ? elf_header64.e_phnum : elf_header32.e_phnum);
  coredump_phdr_t *phdrs = ui->phdrs = memset(malloc(size * sizeof(phdrs[0])), 0, size * sizeof(phdrs[0]));
  if (_64bits)
    {
      coredump_phdr_t *cur = phdrs;
      unsigned i = 0;
      while (i < size)
        {
          Elf64_Phdr hdr64;
          if (read(fd, &hdr64, sizeof(hdr64)) != sizeof(hdr64))
            {
              Debug(0, "Can't read phdrs from '%s'\n", filename);
              goto err;
            }
          cur->p_type   = hdr64.p_type  ;
          cur->p_flags  = hdr64.p_flags ;
          cur->p_offset = hdr64.p_offset;
          cur->p_vaddr  = hdr64.p_vaddr ;
          /*cur->p_paddr  = hdr32.p_paddr ; always 0 */
//TODO: check that and abort if it isn't?
          cur->p_filesz = hdr64.p_filesz;
          cur->p_memsz  = hdr64.p_memsz ;
          cur->p_align  = hdr64.p_align ;
          /* cur->backing_filename = NULL; - done by memset */
          cur->backing_fd = -1;
          cur->backing_filesize = hdr64.p_filesz;
          i++;
          cur++;
        }
    } else {
      coredump_phdr_t *cur = phdrs;
      unsigned i = 0;
      while (i < size)
        {
          Elf32_Phdr hdr32;
          if (read(fd, &hdr32, sizeof(hdr32)) != sizeof(hdr32))
            {
              Debug(0, "Can't read phdrs from '%s'\n", filename);
              goto err;
            }
          cur->p_type   = hdr32.p_type  ;
          cur->p_flags  = hdr32.p_flags ;
          cur->p_offset = hdr32.p_offset;
          cur->p_vaddr  = hdr32.p_vaddr ;
          /*cur->p_paddr  = hdr32.p_paddr ; always 0 */
          cur->p_filesz = hdr32.p_filesz;
          cur->p_memsz  = hdr32.p_memsz ;
          cur->p_align  = hdr32.p_align ;
          /* cur->backing_filename = NULL; - done by memset */
          cur->backing_fd = -1;
          cur->backing_filesize = hdr32.p_memsz;
          i++;
          cur++;
        }
    }

    int ret = _UCD_get_threadinfo(ui, phdrs, size);
    if (ret != UNW_ESUCCESS) {
		Debug(0, "failure retrieving thread info from core file\n");
		goto err;
	}

    ret = _UCD_get_mapinfo(ui, phdrs, size);
    if (ret != UNW_ESUCCESS) {
		Debug(0, "failure retrieving file mapping from core file\n");
		goto err;
	}

	coredump_phdr_t *cur = phdrs;
	for (unsigned i = 0; i < size; ++i)
	  {
		if (cur->p_type == PT_LOAD)
		  {
			Debug(2, " ofs:%08llx va:%08llx filesize:%08llx memsize:%08llx flg:%x",
								(unsigned long long) cur->p_offset,
								(unsigned long long) cur->p_vaddr,
								(unsigned long long) cur->p_filesz,
								(unsigned long long) cur->p_memsz,
								cur->p_flags
			);
			if (cur->p_filesz < cur->p_memsz)
			  {
				Debug(2, " partial");
			  }
			if (cur->p_flags & PF_X)
			  {
				Debug(2, " executable");
			  }
		  }
		Debug(2, "\n");
		cur++;
	  }

    if (ui->n_threads == 0)
      {
        Debug(0, "No NT_PRSTATUS note found in '%s'\n", filename);
        goto err;
      }

    ui->prstatus = &ui->threads[0].prstatus;
    ui->fpregset = &ui->threads[0].fpregset;

  return ui;

 err:
  _UCD_destroy(ui);
  return NULL;
}

int _UCD_get_num_threads(struct UCD_info *ui)
{
  return ui->n_threads;
}

void _UCD_select_thread(struct UCD_info *ui, int n)
{
  if (n >= 0 && n < ui->n_threads) {
    ui->prstatus = &ui->threads[n].prstatus;
    ui->fpregset = &ui->threads[n].fpregset;
  }
}

pid_t _UCD_get_pid(struct UCD_info *ui)
{
#if defined(HAVE_PROCFS_STATUS)
  return ui->prstatus->pid;
#else
  return ui->prstatus->pr_pid;
#endif
}

int _UCD_get_cursig(struct UCD_info *ui)
{
#if defined(HAVE_PROCFS_STATUS)
  return 0;
#else
  return ui->prstatus->pr_cursig;
#endif
}

int _UCD_add_backing_file_at_segment(struct UCD_info *ui, int phdr_no, const char *filename)
{
  if ((unsigned)phdr_no >= ui->phdrs_count)
    {
      Debug(0, "There is no segment %d in this coredump\n", phdr_no);
      return -1;
    }

  struct coredump_phdr *phdr = &ui->phdrs[phdr_no];
  if (phdr->backing_filename)
    {
      Debug(0, "Backing file already added to segment %d\n", phdr_no);
      return -1;
    }

  int fd = open(filename, O_RDONLY);
  if (fd < 0)
    {
      Debug(0, "Can't open '%s'\n", filename);
      return -1;
    }

  phdr->backing_fd = fd;
  phdr->backing_filename = strdup(filename);

  struct stat statbuf;
  if (fstat(fd, &statbuf) != 0)
    {
      Debug(0, "Can't stat '%s'\n", filename);
      goto err;
    }
  phdr->backing_filesize = (uoff_t)statbuf.st_size;

  if (phdr->p_flags != (PF_X | PF_R))
    {
      Debug(1, "Note: phdr[%u] is not r-x: flags are 0x%x\n",
                        phdr_no, phdr->p_flags);
    }

  if (phdr->backing_filesize > phdr->p_memsz)
    {
      /* This is expected */
      Debug(2, "Note: phdr[%u] is %lld bytes, file is larger: %lld bytes\n",
                        phdr_no,
                        (unsigned long long)phdr->p_memsz,
                        (unsigned long long)phdr->backing_filesize
      );
    }
//TODO: else loudly complain? Maybe even fail?

  /* Success */
  return 0;

 err:
  if (phdr->backing_fd >= 0)
    {
      close(phdr->backing_fd);
      phdr->backing_fd = -1;
    }
  free(phdr->backing_filename);
  phdr->backing_filename = NULL;
  return -1;
}

int _UCD_add_backing_file_at_vaddr(struct UCD_info *ui,
                                   unsigned long vaddr,
                                   const char *filename)
{
  unsigned i;
  for (i = 0; i < ui->phdrs_count; i++)
    {
      struct coredump_phdr *phdr = &ui->phdrs[i];
      if (phdr->p_vaddr != vaddr)
        continue;
      /* It seems to match. Add it. */
      return _UCD_add_backing_file_at_segment(ui, i, filename);
    }
  return -1;
}
