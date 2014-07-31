
/*--------------------------------------------------------------------*/
/*--- Reading of syms & debug info from ELF .so/executable files.  ---*/
/*---                                                    readelf.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Valgrind, a dynamic binary instrumentation
   framework.

   Copyright (C) 2000-2013 Julian Seward 
      jseward@acm.org

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#if defined(VGO_linux)

#include "pub_core_basics.h"
#include "pub_core_vki.h"
#include "pub_core_debuginfo.h"
#include "pub_core_libcbase.h"
#include "pub_core_libcprint.h"
#include "pub_core_libcassert.h"
#include "pub_core_machine.h"      /* VG_ELF_CLASS */
#include "pub_core_options.h"
#include "pub_core_oset.h"
#include "pub_core_tooliface.h"    /* VG_(needs) */
#include "pub_core_xarray.h"
#include "priv_misc.h"             /* dinfo_zalloc/free/strdup */
#include "priv_image.h"
#include "priv_d3basics.h"
#include "priv_tytypes.h"
#include "priv_storage.h"
#include "priv_readelf.h"          /* self */
#include "priv_readdwarf.h"        /* 'cos ELF contains DWARF */
#include "priv_readdwarf3.h"
#include "priv_readstabs.h"        /* and stabs, if we're unlucky */

/* --- !!! --- EXTERNAL HEADERS start --- !!! --- */
#include <elf.h>
/* --- !!! --- EXTERNAL HEADERS end --- !!! --- */

/*------------------------------------------------------------*/
/*--- 32/64-bit parameterisation                           ---*/
/*------------------------------------------------------------*/

/* For all the ELF macros and types which specify '32' or '64',
   select the correct variant for this platform and give it
   an 'XX' name.  Then use the 'XX' variant consistently in
   the rest of this file. 
*/
#if VG_WORDSIZE == 4
#  define  ElfXX_Ehdr     Elf32_Ehdr
#  define  ElfXX_Shdr     Elf32_Shdr
#  define  ElfXX_Phdr     Elf32_Phdr
#  define  ElfXX_Nhdr     Elf32_Nhdr
#  define  ElfXX_Sym      Elf32_Sym
#  define  ElfXX_Off      Elf32_Off
#  define  ElfXX_Word     Elf32_Word
#  define  ElfXX_Addr     Elf32_Addr
#  define  ElfXX_Dyn      Elf32_Dyn
#  define  ELFXX_ST_BIND  ELF32_ST_BIND
#  define  ELFXX_ST_TYPE  ELF32_ST_TYPE

#elif VG_WORDSIZE == 8
#  define  ElfXX_Ehdr     Elf64_Ehdr
#  define  ElfXX_Shdr     Elf64_Shdr
#  define  ElfXX_Phdr     Elf64_Phdr
#  define  ElfXX_Nhdr     Elf64_Nhdr
#  define  ElfXX_Sym      Elf64_Sym
#  define  ElfXX_Off      Elf64_Off
#  define  ElfXX_Word     Elf64_Word
#  define  ElfXX_Addr     Elf64_Addr
#  define  ElfXX_Dyn      Elf64_Dyn
#  define  ELFXX_ST_BIND  ELF64_ST_BIND
#  define  ELFXX_ST_TYPE  ELF64_ST_TYPE

#else
# error "VG_WORDSIZE should be 4 or 8"
#endif


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Read symbol table and line info from ELF files.      ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

/* readelf.c parses ELF files and acquires symbol table info from
   them.  It calls onwards to readdwarf.c to read DWARF2/3 line number
   and call frame info found. */

/* Identify an ELF object file by peering at the first few bytes of
   it. */

Bool ML_(is_elf_object_file)( void* image, SizeT n_image, Bool rel_ok )
{
   ElfXX_Ehdr* ehdr = (ElfXX_Ehdr*)image;
   Int ok = 1;

   if (n_image < sizeof(ElfXX_Ehdr))
      return False;

   ok &= (ehdr->e_ident[EI_MAG0] == 0x7F
          && ehdr->e_ident[EI_MAG1] == 'E'
          && ehdr->e_ident[EI_MAG2] == 'L'
          && ehdr->e_ident[EI_MAG3] == 'F');
   ok &= (ehdr->e_ident[EI_CLASS] == VG_ELF_CLASS
          && ehdr->e_ident[EI_DATA] == VG_ELF_DATA2XXX
          && ehdr->e_ident[EI_VERSION] == EV_CURRENT);
   ok &= (ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN
          || (rel_ok && ehdr->e_type == ET_REL));
   ok &= (ehdr->e_machine == VG_ELF_MACHINE);
   ok &= (ehdr->e_version == EV_CURRENT);
   ok &= (ehdr->e_shstrndx != SHN_UNDEF);
   ok &= (ehdr->e_shoff != 0 && ehdr->e_shnum != 0);
   ok &= ((ehdr->e_phoff != 0 && ehdr->e_phnum != 0)
          || ehdr->e_type == ET_REL);

   return ok ? True : False;
}


/* The same thing, but operating on a DiImage instead. */

static Bool is_elf_object_file_by_DiImage( DiImage* img, Bool rel_ok )
{
   /* Be sure this doesn't make the frame too big. */
   vg_assert(sizeof(ElfXX_Ehdr) <= 512);

   ElfXX_Ehdr ehdr;
   if (!ML_(img_valid)(img, 0, sizeof(ehdr)))
      return False;

   ML_(img_get)(&ehdr, img, 0, sizeof(ehdr));
   return ML_(is_elf_object_file)( &ehdr, sizeof(ehdr), rel_ok );
}


/* Show a raw ELF symbol, given its in-image address and name. */

static
void show_raw_elf_symbol ( DiImage* strtab_img,
                           Int i, 
                           ElfXX_Sym* sym,
                           DiOffT sym_name_ioff, Addr sym_svma,
                           Bool ppc64_linux_format )
{
   const HChar* space = ppc64_linux_format ? "                  " : "";
   VG_(printf)("raw symbol [%4d]: ", i);
   switch (ELFXX_ST_BIND(sym->st_info)) {
      case STB_LOCAL:  VG_(printf)("LOC "); break;
      case STB_GLOBAL: VG_(printf)("GLO "); break;
      case STB_WEAK:   VG_(printf)("WEA "); break;
      case STB_LOPROC: VG_(printf)("lop "); break;
      case STB_HIPROC: VG_(printf)("hip "); break;
      default:         VG_(printf)("??? "); break;
   }
   switch (ELFXX_ST_TYPE(sym->st_info)) {
      case STT_NOTYPE:  VG_(printf)("NOT "); break;
      case STT_OBJECT:  VG_(printf)("OBJ "); break;
      case STT_FUNC:    VG_(printf)("FUN "); break;
      case STT_SECTION: VG_(printf)("SEC "); break;
      case STT_FILE:    VG_(printf)("FIL "); break;
      case STT_LOPROC:  VG_(printf)("lop "); break;
      case STT_HIPROC:  VG_(printf)("hip "); break;
      default:          VG_(printf)("??? "); break;
   }
   HChar* sym_name = NULL;
   if (sym->st_name)
      sym_name = ML_(img_strdup)(strtab_img, "di.sres.1", sym_name_ioff);
   VG_(printf)(": svma %#010lx, %ssz %4ld  %s\n",
               sym_svma, space, sym->st_size + 0UL,
               (sym_name ? sym_name : "NONAME") ); 
   if (sym_name)
      ML_(dinfo_free)(sym_name);
}


/* Decide whether SYM is something we should collect, and if so, copy
   relevant info to the _OUT arguments.  For {x86,amd64,ppc32}-linux
   this is straightforward - the name, address, size are copied out
   unchanged.

   There is a bit of a kludge re data symbols (see KLUDGED BSS CHECK
   below): we assume that the .bss is mapped immediately after .data,
   and so accept any data symbol which exists in the range [start of
   .data, size of .data + size of .bss).  I don't know if this is
   really correct/justifiable, or not.

   For ppc64-linux it's more complex.  If the symbol is seen to be in
   the .opd section, it is taken to be a function descriptor, and so
   a dereference is attempted, in order to get hold of the real entry
   point address.  Also as part of the dereference, there is an attempt
   to calculate the TOC pointer (R2 value) associated with the symbol.

   To support the ppc64-linux pre-"dotless" ABI (prior to gcc 4.0.0),
   if the symbol is seen to be outside the .opd section and its name
   starts with a dot, an .opd deference is not attempted, and no TOC
   pointer is calculated, but the the leading dot is removed from the
   name.

   As a result, on ppc64-linux, the caller of this function may have
   to piece together the real size, address, name of the symbol from
   multiple calls to this function.  Ugly and confusing.
*/
static 
Bool get_elf_symbol_info ( 
        /* INPUTS */
        struct _DebugInfo* di, /* containing DebugInfo */
        ElfXX_Sym* sym,        /* ELF symbol */
        DiOffT     sym_name_ioff, /* name, may be absent (DiOffT_INVALID) */
        DiSlice*   escn_strtab,   /* holds the name */
        Addr       sym_svma,   /* address as stated in the object file */
        Bool       symtab_in_debug, /* symbol table is in the debug file */
        DiSlice*   escn_opd,   /* the .opd (ppc64-linux only) */
        PtrdiffT   opd_bias,   /* for biasing AVMAs found in .opd */
        /* OUTPUTS */
        DiOffT* sym_name_out_ioff, /* name (in strtab) we should record */
        Addr*   sym_avma_out,   /* addr we should record */
        Int*    sym_size_out,   /* symbol size */
        Addr*   sym_tocptr_out, /* ppc64-linux only: R2 value to be
                                   used on entry */
        Bool*   from_opd_out,   /* ppc64-linux only: did we deref an
                                  .opd entry? */
        Bool*   is_text_out,    /* is this a text symbol? */
        Bool*   is_ifunc        /* is this a  STT_GNU_IFUNC function ?*/
     )
{
   Bool plausible;
#  if defined(VGP_ppc64_linux)
   Bool is_in_opd;
#  endif
   Bool in_text, in_data, in_sdata, in_rodata, in_bss, in_sbss;
   Addr text_svma, data_svma, sdata_svma, rodata_svma, bss_svma, sbss_svma;
   PtrdiffT text_bias, data_bias, sdata_bias, rodata_bias, bss_bias, sbss_bias;

   /* Set defaults */
   *sym_name_out_ioff = sym_name_ioff;
   *sym_avma_out      = sym_svma; /* we will bias this shortly */
   *is_text_out       = True;
   *sym_tocptr_out    = 0; /* unknown/inapplicable */
   *from_opd_out      = False;
   *is_ifunc          = False;
   /* Get the symbol size, but restrict it to fit in a signed 32 bit
      int.  Also, deal with the stupid case of negative size by making
      the size be 1.  Note that sym->st_size has type UWord,
      effectively. */
   { Word size_tmp = (Word)sym->st_size;
     Word max_Int  = (1LL << 31) - 1;
     if (size_tmp < 0)       size_tmp = 1;
     if (size_tmp > max_Int) size_tmp = max_Int;
     *sym_size_out = (Int)size_tmp;
   }
   /* After this point refer only to *sym_size_out and not to
      sym->st_size. */

   /* Figure out if we're interested in the symbol.  Firstly, is it of
      the right flavour?  */
   plausible 
      = (ELFXX_ST_BIND(sym->st_info) == STB_GLOBAL 
         || ELFXX_ST_BIND(sym->st_info) == STB_LOCAL 
         || ELFXX_ST_BIND(sym->st_info) == STB_WEAK
        )
        &&
        (ELFXX_ST_TYPE(sym->st_info) == STT_FUNC 
         || ELFXX_ST_TYPE(sym->st_info) == STT_OBJECT
#        ifdef STT_GNU_IFUNC
         || ELFXX_ST_TYPE(sym->st_info) == STT_GNU_IFUNC
#        endif
        );

   /* Work out the svma and bias for each section as it will appear in
      addresses in the symbol table. */
   if (symtab_in_debug) {
      text_svma = di->text_debug_svma;
      text_bias = di->text_debug_bias;
      data_svma = di->data_debug_svma;
      data_bias = di->data_debug_bias;
      sdata_svma = di->sdata_debug_svma;
      sdata_bias = di->sdata_debug_bias;
      rodata_svma = di->rodata_debug_svma;
      rodata_bias = di->rodata_debug_bias;
      bss_svma = di->bss_debug_svma;
      bss_bias = di->bss_debug_bias;
      sbss_svma = di->sbss_debug_svma;
      sbss_bias = di->sbss_debug_bias;
   } else {
      text_svma = di->text_svma;
      text_bias = di->text_bias;
      data_svma = di->data_svma;
      data_bias = di->data_bias;
      sdata_svma = di->sdata_svma;
      sdata_bias = di->sdata_bias;
      rodata_svma = di->rodata_svma;
      rodata_bias = di->rodata_bias;
      bss_svma = di->bss_svma;
      bss_bias = di->bss_bias;
      sbss_svma = di->sbss_svma;
      sbss_bias = di->sbss_bias;
   }

   /* Now bias sym_avma_out accordingly by figuring out exactly which
      section the symbol is from and bias accordingly.  Screws up if
      the previously deduced section svma address ranges are wrong. */
   if (di->text_present
       && di->text_size > 0
       && sym_svma >= text_svma 
       && sym_svma < text_svma + di->text_size) {
      *is_text_out = True;
      *sym_avma_out += text_bias;
   } else
   if (di->data_present
       && di->data_size > 0
       && sym_svma >= data_svma 
       && sym_svma < data_svma + di->data_size) {
      *is_text_out = False;
      *sym_avma_out += data_bias;
   } else
   if (di->sdata_present
       && di->sdata_size > 0
       && sym_svma >= sdata_svma 
       && sym_svma < sdata_svma + di->sdata_size) {
      *is_text_out = False;
      *sym_avma_out += sdata_bias;
   } else
   if (di->rodata_present
       && di->rodata_size > 0
       && sym_svma >= rodata_svma 
       && sym_svma < rodata_svma + di->rodata_size) {
      *is_text_out = False;
      *sym_avma_out += rodata_bias;
   } else
   if (di->bss_present
       && di->bss_size > 0
       && sym_svma >= bss_svma 
       && sym_svma < bss_svma + di->bss_size) {
      *is_text_out = False;
      *sym_avma_out += bss_bias;
   } else
   if (di->sbss_present
       && di->sbss_size > 0
       && sym_svma >= sbss_svma 
       && sym_svma < sbss_svma + di->sbss_size) {
      *is_text_out = False;
      *sym_avma_out += sbss_bias;
   } else {
      /* Assume it's in .text.  Is this a good idea? */
      *is_text_out = True;
      *sym_avma_out += text_bias;
   }

#  ifdef STT_GNU_IFUNC
   /* Check for indirect functions. */
   if (*is_text_out
       && ELFXX_ST_TYPE(sym->st_info) == STT_GNU_IFUNC) {
       *is_ifunc = True;
   }
#  endif

#  if defined(VGP_ppc64_linux)
   /* Allow STT_NOTYPE in the very special case where we're running on
      ppc64-linux and the symbol is one which the .opd-chasing hack
      below will chase. */
   if (!plausible
       && *is_text_out
       && ELFXX_ST_TYPE(sym->st_info) == STT_NOTYPE
       && *sym_size_out > 0
       && di->opd_present
       && di->opd_size > 0
       && *sym_avma_out >= di->opd_avma
       && *sym_avma_out <  di->opd_avma + di->opd_size)
      plausible = True;
#  endif

   if (!plausible)
      return False;

   /* Ignore if nameless. */
   if (sym_name_ioff == DiOffT_INVALID
       || /* VG_(strlen)(sym_name) == 0 */
          /* equivalent but cheaper ... */
          ML_(img_get_UChar)(escn_strtab->img, sym_name_ioff) == '\0') {
      if (TRACE_SYMTAB_ENABLED) {
         HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                           "di.gesi.1", sym_name_ioff);
         TRACE_SYMTAB("    ignore -- nameless: %s\n", sym_name);
         if (sym_name) ML_(dinfo_free)(sym_name);
      }
      return False;
   }

   /* Ignore if zero-sized.  Except on Android:

      On Android 2.3.5, some of the symbols that Memcheck needs to
      intercept (for noise reduction purposes) have zero size, due to
      lack of .size directives in handwritten assembly sources.  So we
      can't reject them out of hand -- instead give them a bogusly
      large size and let canonicaliseSymtab trim them so they don't
      overlap any following symbols.  At least the following symbols
      are known to be affected:

      in /system/lib/libc.so: strlen strcmp strcpy memcmp memcpy
      in /system/bin/linker:  __dl_strcmp __dl_strlen
   */
   if (*sym_size_out == 0) {
#     if defined(VGPV_arm_linux_android) || defined(VGPV_x86_linux_android)
      *sym_size_out = 2048;
#     else
      if (TRACE_SYMTAB_ENABLED) {
         HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                           "di.gesi.2", sym_name_ioff);
         TRACE_SYMTAB("    ignore -- size=0: %s\n", sym_name);
         if (sym_name) ML_(dinfo_free)(sym_name);
      }
      return False;
#     endif
   }

   /* This seems to significantly reduce the number of junk
      symbols, and particularly reduces the number of
      overlapping address ranges.  Don't ask me why ... */
   if ((Int)sym->st_value == 0) {
      if (TRACE_SYMTAB_ENABLED) {
         HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                           "di.gesi.3", sym_name_ioff);
         TRACE_SYMTAB( "    ignore -- valu=0: %s\n", sym_name);
         if (sym_name) ML_(dinfo_free)(sym_name);
      }
      return False;
   }

   /* If it's apparently in a GOT or PLT, it's really a reference to a
      symbol defined elsewhere, so ignore it. */
   if (di->got_present
       && di->got_size > 0
       && *sym_avma_out >= di->got_avma 
       && *sym_avma_out <  di->got_avma + di->got_size) {
      if (TRACE_SYMTAB_ENABLED) {
         HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                           "di.gesi.4", sym_name_ioff);
         TRACE_SYMTAB("    ignore -- in GOT: %s\n", sym_name);
         if (sym_name) ML_(dinfo_free)(sym_name);
      }
      return False;
   }
   if (di->plt_present
       && di->plt_size > 0
       && *sym_avma_out >= di->plt_avma
       && *sym_avma_out <  di->plt_avma + di->plt_size) {
      if (TRACE_SYMTAB_ENABLED) {
         HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                           "di.gesi.5", sym_name_ioff);
         TRACE_SYMTAB("    ignore -- in PLT: %s\n", sym_name);
         if (sym_name) ML_(dinfo_free)(sym_name);
      }
      return False;
   }

   /* ppc64-linux nasty hack: if the symbol is in an .opd section,
      then really what we have is the address of a function
      descriptor.  So use the first word of that as the function's
      text.

      See thread starting at
      http://gcc.gnu.org/ml/gcc-patches/2004-08/msg00557.html
   */
#  if defined(VGP_ppc64_linux)
   is_in_opd = False;
#  endif

   if (di->opd_present
       && di->opd_size > 0
       && *sym_avma_out >= di->opd_avma
       && *sym_avma_out <  di->opd_avma + di->opd_size) {
#     if !defined(VGP_ppc64_linux)
      if (TRACE_SYMTAB_ENABLED) {
         HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                           "di.gesi.6", sym_name_ioff);
         TRACE_SYMTAB("    ignore -- in OPD: %s\n", sym_name);
         if (sym_name) ML_(dinfo_free)(sym_name);
      }
      return False;
#     else
      Int    offset_in_opd;
      Bool   details = 1||False;

      if (details)
         TRACE_SYMTAB("opdXXX: opd_bias %p, sym_svma_out %p\n", 
                      (void*)(opd_bias), (void*)*sym_avma_out);

      if (!VG_IS_8_ALIGNED(*sym_avma_out)) {
         if (TRACE_SYMTAB_ENABLED) {
            HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                              "di.gesi.6a", sym_name_ioff);
            TRACE_SYMTAB("    ignore -- not 8-aligned: %s\n", sym_name);
            if (sym_name) ML_(dinfo_free)(sym_name);
         }
         return False;
      }

      /* *sym_avma_out is a vma pointing into the .opd section.  We
         know the vma of the opd section start, so we can figure out
         how far into the opd section this is. */

      offset_in_opd = (Addr)(*sym_avma_out) - (Addr)(di->opd_avma);
      if (offset_in_opd < 0 || offset_in_opd >= di->opd_size) {
         if (TRACE_SYMTAB_ENABLED) {
            HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                              "di.gesi.6a", sym_name_ioff);
            TRACE_SYMTAB("    ignore -- invalid OPD offset: %s\n", sym_name);
            if (sym_name) ML_(dinfo_free)(sym_name);
         }
         return False;
      }

      /* Now we want to know what's at that offset in the .opd
         section.  We can't look in the running image since it won't
         necessarily have been mapped.  But we can consult the oimage.
         opd_img is the start address of the .opd in the oimage.
         Hence: */

      ULong fn_descr[2]; /* is actually 3 words, but we need only 2 */
      if (!ML_(img_valid)(escn_opd->img, escn_opd->ioff + offset_in_opd,
                          sizeof(fn_descr))) {
         if (TRACE_SYMTAB_ENABLED) {
            HChar* sym_name = ML_(img_strdup)(escn_strtab->img,
                                              "di.gesi.6b", sym_name_ioff);
            TRACE_SYMTAB("    ignore -- invalid OPD fn_descr offset: %s\n",
                         sym_name);
            if (sym_name) ML_(dinfo_free)(sym_name);

         }
         return False;
      }

      /* This can't fail now, because we just checked the offset
         above. */
      ML_(img_get)(&fn_descr[0], escn_opd->img,
                   escn_opd->ioff + offset_in_opd, sizeof(fn_descr));

      if (details) 
         TRACE_SYMTAB("opdXXY: offset %d,  fn_descr %p\n", 
                      offset_in_opd, fn_descr);
      if (details) 
         TRACE_SYMTAB("opdXXZ: *fn_descr %p\n", (void*)(fn_descr[0]));

      /* opd_bias is the what we have to add to SVMAs found in .opd to
         get plausible .text AVMAs for the entry point, and .data
         AVMAs (presumably) for the TOC locations.  We use the caller
         supplied value (which is di->text_bias) for both of these.
         Not sure why that is correct - it seems to work, and sounds
         OK for fn_descr[0], but surely we need to use the data bias
         and not the text bias for fn_descr[1] ?  Oh Well.
      */
      *sym_avma_out   = fn_descr[0] + opd_bias;
      *sym_tocptr_out = fn_descr[1] + opd_bias;
      *from_opd_out   = True;
      is_in_opd = True;

      /* Do a final sanity check: if the symbol falls outside the
         DebugInfo's mapped range, ignore it.  Since *sym_avma_out has
         been updated, that can be achieved simply by falling through
         to the test below. */

#     endif /* ppc64-linux nasty hack */
   }

   /* Here's yet another ppc64-linux hack.  Get rid of leading dot if
      the symbol is outside .opd. */
#  if defined(VGP_ppc64_linux)
   if (di->opd_size > 0
       && !is_in_opd
       && *sym_name_out_ioff != DiOffT_INVALID
       && ML_(img_get_UChar)(escn_strtab->img, *sym_name_out_ioff) == '.') {
      vg_assert(!(*from_opd_out));
      (*sym_name_out_ioff)++;
   }
#  endif

   /* If no part of the symbol falls within the mapped range,
      ignore it. */
   
   in_text 
      = di->text_present
        && di->text_size > 0
        && !(*sym_avma_out + *sym_size_out <= di->text_avma
             || *sym_avma_out >= di->text_avma + di->text_size);

   in_data 
      = di->data_present
        && di->data_size > 0
        && !(*sym_avma_out + *sym_size_out <= di->data_avma
             || *sym_avma_out >= di->data_avma + di->data_size);

   in_sdata 
      = di->sdata_present
        && di->sdata_size > 0
        && !(*sym_avma_out + *sym_size_out <= di->sdata_avma
             || *sym_avma_out >= di->sdata_avma + di->sdata_size);

   in_rodata 
      = di->rodata_present
        && di->rodata_size > 0
        && !(*sym_avma_out + *sym_size_out <= di->rodata_avma
             || *sym_avma_out >= di->rodata_avma + di->rodata_size);

   in_bss 
      = di->bss_present
        && di->bss_size > 0
        && !(*sym_avma_out + *sym_size_out <= di->bss_avma
             || *sym_avma_out >= di->bss_avma + di->bss_size);

   in_sbss 
      = di->sbss_present
        && di->sbss_size > 0
        && !(*sym_avma_out + *sym_size_out <= di->sbss_avma
             || *sym_avma_out >= di->sbss_avma + di->sbss_size);


   if (*is_text_out) {
      /* This used to reject any symbol falling outside the text
         segment ("if (!in_text) ...").  Now it is relaxed slightly,
         to reject only symbols which fall outside the area mapped
         r-x.  This is in accordance with r7427.  See
         "Comment_Regarding_Text_Range_Checks" in storage.c for
         background. */
      Bool in_rx;
      vg_assert(di->fsm.have_rx_map);
      /* This could actually wrap around and cause
         ML_(find_rx_mapping) to assert.  But that seems so unlikely,
         let's wait for it to happen before fixing it. */
      in_rx = (ML_(find_rx_mapping)(di, *sym_avma_out,
                                    *sym_avma_out + *sym_size_out) != NULL);
      if (in_text)
         vg_assert(in_rx);
      if (!in_rx) {
         TRACE_SYMTAB(
            "ignore -- %#lx .. %#lx outside .text svma range %#lx .. %#lx\n",
            *sym_avma_out, *sym_avma_out + *sym_size_out,
            di->text_avma,
            di->text_avma + di->text_size);
         return False;
      }
   } else {
     if (!(in_data || in_sdata || in_rodata || in_bss || in_sbss)) {
         TRACE_SYMTAB(
            "ignore -- %#lx .. %#lx outside .data / .sdata / .rodata "
            "/ .bss / .sbss svma ranges\n",
            *sym_avma_out, *sym_avma_out + *sym_size_out);
         return False;
      }
   }

#  if defined(VGP_ppc64_linux)
   /* It's crucial that we never add symbol addresses in the .opd
      section.  This would completely mess up function redirection and
      intercepting.  This assert ensures that any symbols that make it
      into the symbol table on ppc64-linux don't point into .opd. */
   if (di->opd_present && di->opd_size > 0) {
      vg_assert(*sym_avma_out + *sym_size_out <= di->opd_avma
                || *sym_avma_out >= di->opd_avma + di->opd_size);
   }
#  endif

   /* Acquire! */
   return True;
}


/* Read an ELF symbol table (normal or dynamic).  This one is for the
   "normal" case ({x86,amd64,ppc32,arm,mips32,mips64}-linux). */
static
__attribute__((unused)) /* not referred to on all targets */
void read_elf_symtab__normal( 
        struct _DebugInfo* di, const HChar* tab_name,
        DiSlice*   escn_symtab,
        DiSlice*   escn_strtab,
        DiSlice*   escn_opd, /* ppc64-linux only */ 
        Bool       symtab_in_debug
     )
{
   if (escn_strtab->img == NULL || escn_symtab->img == NULL) {
      HChar buf[80];
      vg_assert(VG_(strlen)(tab_name) < 40);
      VG_(sprintf)(buf, "   object doesn't have a %s", tab_name);
      ML_(symerr)(di, False, buf);
      return;
   }

   TRACE_SYMTAB("\n--- Reading (ELF, standard) %s (%lld entries) ---\n",
                tab_name, escn_symtab->szB/sizeof(ElfXX_Sym) );

   /* Perhaps should start at i = 1; ELF docs suggest that entry
      0 always denotes 'unknown symbol'. */
   Word i;
   for (i = 1; i < (Word)(escn_symtab->szB/sizeof(ElfXX_Sym)); i++) {
      ElfXX_Sym sym;
      ML_(img_get)(&sym, escn_symtab->img,
                   escn_symtab->ioff + i * sizeof(ElfXX_Sym), sizeof(sym));
      DiOffT sym_name = escn_strtab->ioff + sym.st_name;
      Addr   sym_svma = sym.st_value;

      if (di->trace_symtab)
        show_raw_elf_symbol(escn_strtab->img, i,
                            &sym, sym_name, sym_svma, False);

      Addr   sym_avma_really = 0;
      Int    sym_size = 0;
      Addr   sym_tocptr = 0;
      Bool   from_opd = False, is_text = False, is_ifunc = False;
      DiOffT sym_name_really = DiOffT_INVALID;
      if (get_elf_symbol_info(di, &sym, sym_name, escn_strtab, 
                              sym_svma, symtab_in_debug,
                              escn_opd, di->text_bias,
                              &sym_name_really, 
                              &sym_avma_really,
                              &sym_size,
                              &sym_tocptr,
                              &from_opd, &is_text, &is_ifunc)) {

         DiSym  disym;
         VG_(memset)(&disym, 0, sizeof(disym));
         HChar* cstr = ML_(img_strdup)(escn_strtab->img,
                                       "di.res__n.1", sym_name_really);
         disym.addr      = sym_avma_really;
         disym.tocptr    = sym_tocptr;
         disym.pri_name  = ML_(addStr) ( di, cstr, -1 );
         disym.sec_names = NULL;
         disym.size      = sym_size;
         disym.isText    = is_text;
         disym.isIFunc   = is_ifunc;
         if (cstr) { ML_(dinfo_free)(cstr); cstr = NULL; }
         vg_assert(disym.pri_name);
         vg_assert(disym.tocptr == 0); /* has no role except on ppc64-linux */
         ML_(addSym) ( di, &disym );

         if (TRACE_SYMTAB_ENABLED) {
            TRACE_SYMTAB("    rec(%c) [%4ld]:          "
                         "  val %#010lx, sz %4d  %s\n",
                         is_text ? 't' : 'd',
                         i,
                         disym.addr,
                         (Int)disym.size,
                         (HChar*)disym.pri_name
            );
         }

      }
   }
}


/* Read an ELF symbol table (normal or dynamic).  This one is for
   ppc64-linux, which requires special treatment. */

typedef
   struct { 
      Addr   addr;
      DiOffT name;
      /* We have to store also the DiImage* so as to give context for
         |name|.  This is not part of the key (in terms of lookup) but
         there's no easy other way to do this.  Ugly. */
      DiImage* img;
   }
   TempSymKey;

typedef
   struct {
      TempSymKey key;
      Addr       tocptr;
      Int        size;
      Bool       from_opd;
      Bool       is_text;
      Bool       is_ifunc;
   }
   TempSym;

static Word cmp_TempSymKey ( TempSymKey* key1, TempSym* elem2 )
{
   /* Stay sane ... */
   vg_assert(key1->img == elem2->key.img);
   vg_assert(key1->img != NULL);
   if (key1->addr < elem2->key.addr) return -1;
   if (key1->addr > elem2->key.addr) return 1;
   vg_assert(key1->name != DiOffT_INVALID);
   vg_assert(elem2->key.name != DiOffT_INVALID);
   return (Word)ML_(img_strcmp)(key1->img, key1->name, elem2->key.name);
}

static
__attribute__((unused)) /* not referred to on all targets */
void read_elf_symtab__ppc64_linux( 
        struct _DebugInfo* di, const HChar* tab_name,
        DiSlice*   escn_symtab,
        DiSlice*   escn_strtab,
        DiSlice*   escn_opd, /* ppc64-linux only */ 
        Bool       symtab_in_debug
     )
{
   Word        i;
   Int         old_size;
   Bool        modify_size, modify_tocptr;
   OSet       *oset;
   TempSymKey  key;
   TempSym    *elem;
   TempSym    *prev;

   if (escn_strtab->img == NULL || escn_symtab->img == NULL) {
      HChar buf[80];
      vg_assert(VG_(strlen)(tab_name) < 40);
      VG_(sprintf)(buf, "   object doesn't have a %s", tab_name);
      ML_(symerr)(di, False, buf);
      return;
   }

   TRACE_SYMTAB("\n--- Reading (ELF, ppc64-linux) %s (%lld entries) ---\n",
                tab_name, escn_symtab->szB/sizeof(ElfXX_Sym) );

   oset = VG_(OSetGen_Create)( offsetof(TempSym,key), 
                               (OSetCmp_t)cmp_TempSymKey, 
                               ML_(dinfo_zalloc), "di.respl.1",
                               ML_(dinfo_free) );
   vg_assert(oset);

   /* Perhaps should start at i = 1; ELF docs suggest that entry
      0 always denotes 'unknown symbol'. */
   for (i = 1; i < (Word)(escn_symtab->szB/sizeof(ElfXX_Sym)); i++) {
      ElfXX_Sym sym;
      ML_(img_get)(&sym, escn_symtab->img,
                   escn_symtab->ioff + i * sizeof(ElfXX_Sym), sizeof(sym));
      DiOffT sym_name = escn_strtab->ioff + sym.st_name;
      Addr   sym_svma = sym.st_value;

      if (di->trace_symtab)
         show_raw_elf_symbol(escn_strtab->img, i,
                             &sym, sym_name, sym_svma, True);

      Addr   sym_avma_really = 0;
      Int    sym_size = 0;
      Addr   sym_tocptr = 0;
      Bool   from_opd = False, is_text = False, is_ifunc = False;
      DiOffT sym_name_really = DiOffT_INVALID;
      DiSym  disym;
      VG_(memset)(&disym, 0, sizeof(disym));
      if (get_elf_symbol_info(di, &sym, sym_name, escn_strtab,
                              sym_svma, symtab_in_debug,
                              escn_opd, di->text_bias,
                              &sym_name_really, 
                              &sym_avma_really,
                              &sym_size,
                              &sym_tocptr,
                              &from_opd, &is_text, &is_ifunc)) {

         /* Check if we've seen this (name,addr) key before. */
         key.addr = sym_avma_really;
         key.name = sym_name_really;
         key.img  = escn_strtab->img;
         prev = VG_(OSetGen_Lookup)( oset, &key );

         if (prev) {

            /* Seen it before.  Fold in whatever new info we can. */
            modify_size   = False;
            modify_tocptr = False;
            old_size   = 0;

            if (prev->from_opd && !from_opd 
                && (prev->size == 24 || prev->size == 16)
                && sym_size != prev->size) {
               /* Existing one is an opd-redirect, with a bogus size,
                  so the only useful new fact we have is the real size
                  of the symbol. */
               modify_size = True;
               old_size = prev->size;
               prev->size = sym_size;
            }
            else
            if (!prev->from_opd && from_opd
                && (sym_size == 24 || sym_size == 16)) {
               /* Existing one is non-opd, new one is opd.  What we
                  can acquire from the new one is the TOC ptr to be
                  used.  Since the existing sym is non-toc, it
                  shouldn't currently have an known TOC ptr. */
               vg_assert(prev->tocptr == 0);
               modify_tocptr = True;
               prev->tocptr = sym_tocptr;
            }
            else {
               /* ignore. can we do better here? */
            }

            /* Only one or the other is possible (I think) */
            vg_assert(!(modify_size && modify_tocptr));

            if (modify_size && di->trace_symtab) {
               VG_(printf)("    modify (old sz %4d)    "
                           " val %#010lx, toc %#010lx, sz %4d  %lld\n",
                           old_size,
                           prev->key.addr,
                           prev->tocptr,
                           (Int)  prev->size, 
                           (ULong)prev->key.name
               );
            }
            if (modify_tocptr && di->trace_symtab) {
               VG_(printf)("    modify (upd tocptr)     "
                           " val %#010lx, toc %#010lx, sz %4d  %lld\n",
                           prev->key.addr,
                           prev->tocptr,
                           (Int)  prev->size,
                           (ULong)prev->key.name
               );
            }

         } else {

            /* A new (name,addr) key.  Add and continue. */
            elem = VG_(OSetGen_AllocNode)(oset, sizeof(TempSym));
            vg_assert(elem);
            elem->key      = key;
            elem->tocptr   = sym_tocptr;
            elem->size     = sym_size;
            elem->from_opd = from_opd;
            elem->is_text  = is_text;
            elem->is_ifunc = is_ifunc;
            VG_(OSetGen_Insert)(oset, elem);
            if (di->trace_symtab) {
               HChar* str = ML_(img_strdup)(escn_strtab->img, "di.respl.2",
                                            elem->key.name);
               VG_(printf)("   to-oset [%4ld]:          "
                           "  val %#010lx, toc %#010lx, sz %4d  %s\n",
                           i,
                           elem->key.addr,
                           elem->tocptr,
                           (Int)  elem->size,
                           str
               );
               if (str) ML_(dinfo_free)(str);
            }

         }
      }
   }

   /* All the syms that matter are in the oset.  Now pull them out,
      build a "standard" symbol table, and nuke the oset. */

   i = 0;
   VG_(OSetGen_ResetIter)( oset );

   while ( (elem = VG_(OSetGen_Next)(oset)) ) {
      DiSym disym;
      VG_(memset)(&disym, 0, sizeof(disym));
      HChar* cstr = ML_(img_strdup)(escn_strtab->img,
                                    "di.res__ppc64.1", elem->key.name);
      disym.addr      = elem->key.addr;
      disym.tocptr    = elem->tocptr;
      disym.pri_name  = ML_(addStr) ( di, cstr, -1 );
      disym.sec_names = NULL;
      disym.size      = elem->size;
      disym.isText    = elem->is_text;
      disym.isIFunc   = elem->is_ifunc;
      if (cstr) { ML_(dinfo_free)(cstr); cstr = NULL; }
      vg_assert(disym.pri_name != NULL);

      ML_(addSym) ( di, &disym );
      if (di->trace_symtab) {
         VG_(printf)("    rec(%c) [%4ld]:          "
                     "   val %#010lx, toc %#010lx, sz %4d  %s\n",
                     disym.isText ? 't' : 'd',
                     i,
                     disym.addr,
                     disym.tocptr,
                     (Int)   disym.size,
                     (HChar*)disym.pri_name
               );
      }
      i++;
   }

   VG_(OSetGen_Destroy)( oset );
}


/*
 * Look for a build-id in an ELF image. The build-id specification
 * can be found here:
 *
 * http://fedoraproject.org/wiki/RolandMcGrath/BuildID
 *
 * Returned string must be freed by the caller.
 */
static
HChar* find_buildid(DiImage* img, Bool rel_ok, Bool search_shdrs)
{
   HChar* buildid = NULL;

#  ifdef NT_GNU_BUILD_ID
   if (is_elf_object_file_by_DiImage(img, rel_ok)) {
      Word i;

      ElfXX_Ehdr ehdr;
      ML_(img_get)(&ehdr, img, 0, sizeof(ehdr));
      for (i = 0; i < ehdr.e_phnum; i++) {
         ElfXX_Phdr phdr;
         ML_(img_get)(&phdr, img,
                      ehdr.e_phoff + i * ehdr.e_phentsize, sizeof(phdr));

         if (phdr.p_type == PT_NOTE) {
            ElfXX_Off note_ioff = phdr.p_offset;

            while (note_ioff < phdr.p_offset + phdr.p_filesz) {
               ElfXX_Nhdr note;
               ML_(img_get)(&note, img, (DiOffT)note_ioff, sizeof(note));
               DiOffT name_ioff = note_ioff + sizeof(ElfXX_Nhdr);
               DiOffT desc_ioff = name_ioff + ((note.n_namesz + 3) & ~3);
               if (ML_(img_strcmp_c)(img, name_ioff, ELF_NOTE_GNU) == 0
                   && note.n_type == NT_GNU_BUILD_ID) {
                  buildid = ML_(dinfo_zalloc)("di.fbi.1",
                                              note.n_descsz * 2 + 1);
                  Word j;
                  for (j = 0; j < note.n_descsz; j++) {
                     UChar desc_j = ML_(img_get_UChar)(img, desc_ioff + j);
                     VG_(sprintf)(buildid + VG_(strlen)(buildid), 
                                  "%02x", (UInt)desc_j);
                  }
               }

               note_ioff = note_ioff + sizeof(ElfXX_Nhdr)
                                     + ((note.n_namesz + 3) & ~3)
                                     + ((note.n_descsz + 3) & ~3);
            }            
         }
      }

      /* Normally we would only search shdrs for ET_REL files, but when
         we search for a separate .debug file phdrs might not be there
         (they are never loaded) or have been corrupted, so try again
         against shdrs. */
      if (buildid || (!rel_ok && !search_shdrs))
         return buildid;

      for (i = 0; i < ehdr.e_shnum; i++) {
         ElfXX_Shdr shdr;
         ML_(img_get)(&shdr, img,
                      ehdr.e_shoff + i * ehdr.e_shentsize, sizeof(shdr));

         if (shdr.sh_type == SHT_NOTE) {
            ElfXX_Off note_ioff = shdr.sh_offset;

            while (note_ioff < shdr.sh_offset + shdr.sh_size) {
               ElfXX_Nhdr note;
               ML_(img_get)(&note, img, (DiOffT)note_ioff, sizeof(note));
               DiOffT name_ioff = note_ioff + sizeof(ElfXX_Nhdr);
               DiOffT desc_ioff = name_ioff + ((note.n_namesz + 3) & ~3);

               if (ML_(img_strcmp_c)(img, name_ioff, ELF_NOTE_GNU) == 0
                   && note.n_type == NT_GNU_BUILD_ID) {
                  buildid = ML_(dinfo_zalloc)("di.fbi.2",
                                              note.n_descsz * 2 + 1);
                  Word j;
                  for (j = 0; j < note.n_descsz; j++) {
                     UChar desc_j = ML_(img_get_UChar)(img, desc_ioff + j);
                     VG_(sprintf)(buildid + VG_(strlen)(buildid), 
                                  "%02x", (UInt)desc_j);
                  }
               }

               note_ioff = note_ioff + sizeof(ElfXX_Nhdr)
                                     + ((note.n_namesz + 3) & ~3)
                                     + ((note.n_descsz + 3) & ~3);
            }            
         }
      }
   }
#  endif /* def NT_GNU_BUILD_ID */

   return buildid;
}


/* Try and open a separate debug file, ignoring any where the CRC does
   not match the value from the main object file.  Returned DiImage
   must be discarded by the caller.

   If |serverAddr| is NULL, |name| is expected to be a fully qualified
   (absolute) path to the file in the local filesystem.  If
   |serverAddr| is non-NULL, it is expected to be an IPv4 and port
   spec of the form "d.d.d.d:d" or "d.d.d.d", and |name| is expected
   to be a plain filename (no path components at all).
 */
static
DiImage* open_debug_file( const HChar* name, const HChar* buildid, UInt crc,
                          Bool rel_ok, const HChar* serverAddr )
{
   DiImage* dimg 
     = serverAddr ? ML_(img_from_di_server)(name, serverAddr)
                  : ML_(img_from_local_file)(name);
   if (dimg == NULL)
      return NULL;

   if (VG_(clo_verbosity) > 1) {
      if (serverAddr)
         VG_(message)(Vg_DebugMsg, "  Considering %s on server %s ..\n",
                                   name, serverAddr);
      else
         VG_(message)(Vg_DebugMsg, "  Considering %s ..\n", name);
   }

   if (buildid) {
      HChar* debug_buildid = find_buildid(dimg, rel_ok, True);
      if (debug_buildid == NULL || VG_(strcmp)(buildid, debug_buildid) != 0) {
         ML_(img_done)(dimg);
         if (VG_(clo_verbosity) > 1)
            VG_(message)(Vg_DebugMsg, 
               "  .. build-id mismatch (found %s wanted %s)\n", 
               debug_buildid, buildid);
         ML_(dinfo_free)(debug_buildid);
         return NULL;
      }
      ML_(dinfo_free)(debug_buildid);
      if (VG_(clo_verbosity) > 1)
         VG_(message)(Vg_DebugMsg, "  .. build-id is valid\n");
   } else {
      UInt calccrc = ML_(img_calc_gnu_debuglink_crc32)(dimg);
      if (calccrc != crc) {
         ML_(img_done)(dimg);
         if (VG_(clo_verbosity) > 1)
            VG_(message)(Vg_DebugMsg, 
               "  .. CRC mismatch (computed %08x wanted %08x)\n", calccrc, crc);
         return NULL;
      }

      if (VG_(clo_verbosity) > 1)
         VG_(message)(Vg_DebugMsg, "  .. CRC is valid\n");
   }
   
   return dimg;
}


/* Try to find a separate debug file for a given object file.  If
   found, return its DiImage, which should be freed by the caller.  If
   |buildid| is non-NULL, then a debug object matching it is
   acceptable.  If |buildid| is NULL or doesn't specify a findable
   debug object, then we look in various places to find a file with
   the specified CRC.  And if that doesn't work out then we give
   up. */
static
DiImage* find_debug_file( struct _DebugInfo* di,
                          const HChar* objpath, const HChar* buildid,
                          const HChar* debugname, UInt crc, Bool rel_ok )
{
   const HChar* extrapath  = VG_(clo_extra_debuginfo_path);
   const HChar* serverpath = VG_(clo_debuginfo_server);

   DiImage* dimg      = NULL; /* the img that we found */
   HChar*   debugpath = NULL; /* where we found it */

   if (buildid != NULL) {
      debugpath = ML_(dinfo_zalloc)("di.fdf.1",
                                    VG_(strlen)(buildid) + 33);

      VG_(sprintf)(debugpath, "/usr/lib/debug/.build-id/%c%c/%s.debug",
                   buildid[0], buildid[1], buildid + 2);

      dimg = open_debug_file(debugpath, buildid, 0, rel_ok, NULL);
      if (!dimg) {
         ML_(dinfo_free)(debugpath);
         debugpath = NULL;
      }
   }

   if (dimg == NULL && debugname != NULL && !rel_ok) {
      HChar *objdir = ML_(dinfo_strdup)("di.fdf.2", objpath);
      HChar *objdirptr;

      if ((objdirptr = VG_(strrchr)(objdir, '/')) != NULL)
         *objdirptr = '\0';

      debugpath = ML_(dinfo_zalloc)(
                     "di.fdf.3",
                     VG_(strlen)(objdir) + VG_(strlen)(debugname) + 64
                     + (extrapath ? VG_(strlen)(extrapath) : 0)
                     + (serverpath ? VG_(strlen)(serverpath) : 0));

      VG_(sprintf)(debugpath, "%s/%s", objdir, debugname);
      dimg = open_debug_file(debugpath, NULL, crc, rel_ok, NULL);
      if (dimg != NULL) goto dimg_ok;

      VG_(sprintf)(debugpath, "%s/.debug/%s", objdir, debugname);
      dimg = open_debug_file(debugpath, NULL, crc, rel_ok, NULL);
      if (dimg != NULL) goto dimg_ok;
      
      VG_(sprintf)(debugpath, "/usr/lib/debug%s/%s", objdir, debugname);
      dimg = open_debug_file(debugpath, NULL, crc, rel_ok, NULL);
      if (dimg != NULL) goto dimg_ok;

      if (extrapath) {
         VG_(sprintf)(debugpath, "%s%s/%s", extrapath,
                                            objdir, debugname);
         dimg = open_debug_file(debugpath, NULL, crc, rel_ok, NULL);
         if (dimg != NULL) goto dimg_ok;
      }

      if (serverpath) {
         /* When looking on the debuginfo server, always just pass the
            basename. */
         const HChar* basename = debugname;
         if (VG_(strstr)(basename, "/") != NULL) {
            basename = VG_(strrchr)(basename, '/') + 1;
         }
         VG_(sprintf)(debugpath, "%s on %s", basename, serverpath);
         dimg = open_debug_file(basename, NULL, crc, rel_ok, serverpath);
         if (dimg) goto dimg_ok;
      }

      dimg_ok:

      ML_(dinfo_free)(objdir);
   }

   if (dimg != NULL) {
      vg_assert(debugpath);
      TRACE_SYMTAB("\n");
      TRACE_SYMTAB("------ Found a debuginfo file: %s\n", debugpath);
   }

   if (debugpath)
      ML_(dinfo_free)(debugpath);

   return dimg;
}


/* Try to find a separate debug file for a given object file, in a
   hacky and dangerous way: check only the --extra-debuginfo-path and
   the --debuginfo-server.  And don't do a consistency check. */
static
DiImage* find_debug_file_ad_hoc( struct _DebugInfo* di,
                                 const HChar* objpath )
{
   const HChar* extrapath  = VG_(clo_extra_debuginfo_path);
   const HChar* serverpath = VG_(clo_debuginfo_server);

   DiImage* dimg      = NULL; /* the img that we found */
   HChar*   debugpath = NULL; /* where we found it */

   HChar *objdir = ML_(dinfo_strdup)("di.fdfah.1", objpath);
   HChar *objdirptr;

   if ((objdirptr = VG_(strrchr)(objdir, '/')) != NULL)
      *objdirptr = '\0';

   debugpath = ML_(dinfo_zalloc)(
                  "di.fdfah.3",
                  VG_(strlen)(objdir) + 64
                  + (extrapath ? VG_(strlen)(extrapath) : 0)
                  + (serverpath ? VG_(strlen)(serverpath) : 0));

   if (extrapath) {
      VG_(sprintf)(debugpath, "%s/%s", extrapath, objpath);
      dimg = ML_(img_from_local_file)(debugpath);
      if (dimg != NULL) {
         if (VG_(clo_verbosity) > 1) {
            VG_(message)(Vg_DebugMsg, "  Using (POSSIBLY MISMATCHED) %s\n",
                                      debugpath);
         }
         goto dimg_ok;
      }
   }
   if (serverpath) {
      /* When looking on the debuginfo server, always just pass the
         basename. */
      const HChar* basename = objpath;
      if (VG_(strstr)(basename, "/") != NULL) {
         basename = VG_(strrchr)(basename, '/') + 1;
      }
      VG_(sprintf)(debugpath, "%s on %s", basename, serverpath);
      dimg = ML_(img_from_di_server)(basename, serverpath);
      if (dimg != NULL) {
         if (VG_(clo_verbosity) > 1) {
            VG_(message)(Vg_DebugMsg, "  Using (POSSIBLY MISMATCHED) %s\n",
                                      debugpath);
         }
         goto dimg_ok;
      }
   }

   dimg_ok:

   ML_(dinfo_free)(objdir);

   if (dimg != NULL) {
      vg_assert(debugpath);
      TRACE_SYMTAB("\n");
      TRACE_SYMTAB("------ Found an ad_hoc debuginfo file: %s\n", debugpath);
   }

   if (debugpath)
      ML_(dinfo_free)(debugpath);

   return dimg;
}


static DiOffT INDEX_BIS ( DiOffT base, UWord idx, UWord scale ) {
   // This is a bit stupid.  Really, idx and scale ought to be
   // 64-bit quantities, always.
   return base + (DiOffT)idx * (DiOffT)scale;
}


/* Find the file offset corresponding to SVMA by using the program
   headers.  This is taken from binutils-2.17/binutils/readelf.c
   offset_from_vma(). */
static
Word file_offset_from_svma ( /*OUT*/Bool* ok,
                             Addr         svma,
                             DiImage*     img,
                             DiOffT       phdr_ioff,
                             Word         phdr_nent,
                             Word         phdr_ent_szB )
{
   Word i;
   for (i = 0; i < phdr_nent; i++) {
      ElfXX_Phdr seg;
      ML_(img_get)(&seg, img,
                   INDEX_BIS(phdr_ioff, i, phdr_ent_szB), sizeof(seg));
      if (seg.p_type != PT_LOAD)
         continue;
      if (svma >= (seg.p_vaddr & -seg.p_align)
          && svma + 1 <= seg.p_vaddr + seg.p_filesz) {
         *ok = True;
         return svma - seg.p_vaddr + seg.p_offset;
      }
   }
   *ok = False;
   return 0;
}


/* The central function for reading ELF debug info.  For the
   object/exe specified by the DebugInfo, find ELF sections, then read
   the symbols, line number info, file name info, CFA (stack-unwind
   info) and anything else we want, into the tables within the
   supplied DebugInfo.
*/

Bool ML_(read_elf_debug_info) ( struct _DebugInfo* di )
{
   /* This function is long and complex.  That, and the presence of
      nested scopes, means it's not always easy to see which parts are
      in loops/conditionals and which aren't.  To make it easier to
      follow, points executed exactly once -- that is, those which are
      the top level of the function -- are marked TOPLEVEL.
   */
   /* Consistent terminology for local variable names, without which
      it's almost unfollowably complex:

      In which file?
         in the main ELF file         *_m*
         in the debuginfo file        *_d*
         in the alt debuginfo file    *_a*

      What kind of thing?
         _{m,d,a}img       a DiImage*
         _{m,d,a}ioff      an offset in the image (DiOffT)
         _{m,d,a}nent      "number of entries"
         _{m,d,a}ent_szB   "size in bytes of an entry"
         ehdr_{m,d,a}      ELF header
         phdr              Program header
         shdr              Section header
         a_X               a temporary X
         _escn             an DiSlice (elf section info) variable
         szB               size in bytes
   */


   /* TOPLEVEL */
   Bool     res, ok;
   Word     i, j;
   Bool     dynbss_present = False;
   Bool     sdynbss_present = False;

   /* Image for the main ELF file we're working with. */
   DiImage* mimg = NULL;

   /* Ditto for any ELF debuginfo file that we might happen to load. */
   DiImage* dimg = NULL;

   /* Ditto for alternate ELF debuginfo file that we might happen to load. */
   DiImage* aimg = NULL;

   /* ELF header offset for the main file.  Should be zero since the
      ELF header is at start of file. */
   DiOffT   ehdr_mioff = 0;

   /* Program header table image addr, # entries, entry size */
   DiOffT   phdr_mioff    = 0;
   UWord    phdr_mnent    = 0;
   UWord    phdr_ment_szB = 0;

   /* Section header image addr, # entries, entry size.  Also the
      associated string table. */
   DiOffT   shdr_mioff        = 0;
   UWord    shdr_mnent        = 0;
   UWord    shdr_ment_szB     = 0;
   DiOffT   shdr_strtab_mioff = 0;

   /* SVMAs covered by rx and rw segments and corresponding biases.
      Normally each object would provide just one rx and one rw area,
      but various ELF mangling tools create objects with multiple
      such entries, hence the generality. */
   typedef
      struct {
         Addr     svma_base;
         Addr     svma_limit;
         PtrdiffT bias;
         Bool     exec;
      }
      RangeAndBias;

   XArray* /* of RangeAndBias */ svma_ranges = NULL;

   vg_assert(di);
   vg_assert(di->fsm.have_rx_map == True);
   vg_assert(di->fsm.have_rw_map == True);
   vg_assert(di->have_dinfo == False);
   vg_assert(di->fsm.filename);
   vg_assert(!di->symtab);
   vg_assert(!di->loctab);
   vg_assert(!di->cfsi);
   vg_assert(!di->cfsi_exprs);
   vg_assert(!di->strchunks);
   vg_assert(!di->soname);

   {
      Bool has_nonempty_rx = False;
      Bool has_nonempty_rw = False;
      for (i = 0; i < VG_(sizeXA)(di->fsm.maps); i++) {
         struct _DebugInfoMapping* map = VG_(indexXA)(di->fsm.maps, i);
         if (!map->rx && !map->rw)
            continue;
         if (map->rx && map->size > 0)
            has_nonempty_rx = True;
         if (map->rw && map->size > 0)
            has_nonempty_rw = True;
         /* If this doesn't hold true, it means that m_syswrap/m_aspacemgr
            managed to do a mapping where the start isn't page aligned.
            Which sounds pretty bogus to me. */
         vg_assert(VG_IS_PAGE_ALIGNED(map->avma));
      }
      vg_assert(has_nonempty_rx);
      vg_assert(has_nonempty_rw);
   }

   /* ----------------------------------------------------------
      At this point, there is very little information in the
      DebugInfo.  We only know that something that looks like an ELF
      file has been mapped rx-ishly and rw-ishly as recorded in the
      di->fsm.maps array items.  First we examine the file's ELF
      Program Header, and, by comparing that against the di->fsm.maps
      info, try to figure out the AVMAs for the sections we care
      about, that should have been mapped: text, data, sdata, bss,
      got, plt, and toc.
      ---------------------------------------------------------- */

   res = False;

   if (VG_(clo_verbosity) > 1 || VG_(clo_trace_redir))
      VG_(message)(Vg_DebugMsg, "Reading syms from %s\n",
                                di->fsm.filename );

   /* Connect to the primary object image, so that we can read symbols
      and line number info out of it.  It will be disconnected
      immediately thereafter; it is only connected transiently. */
   mimg = ML_(img_from_local_file)(di->fsm.filename);
   if (mimg == NULL) {
      VG_(message)(Vg_UserMsg, "warning: connection to image %s failed\n",
                               di->fsm.filename );
      VG_(message)(Vg_UserMsg, "         no symbols or debug info loaded\n" );
      return False;
   }

   /* Ok, the object image is available.  Now verify that it is a
      valid ELF .so or executable image. */
   ok = is_elf_object_file_by_DiImage(mimg, False);
   if (!ok) {
      ML_(symerr)(di, True, "Invalid ELF Header");
      goto out;
   }

   /* Find where the program and section header tables are, and give
      up if either is missing or outside the image (bogus). */
   ElfXX_Ehdr ehdr_m;
   vg_assert(ehdr_mioff == 0); // ensured by its initialisation
   ok = ML_(img_valid)(mimg, ehdr_mioff, sizeof(ehdr_m));
   vg_assert(ok); // ML_(is_elf_object_file) should ensure this
   ML_(img_get)(&ehdr_m, mimg, ehdr_mioff, sizeof(ehdr_m));

   phdr_mioff    = ehdr_mioff + ehdr_m.e_phoff;
   phdr_mnent    = ehdr_m.e_phnum;
   phdr_ment_szB = ehdr_m.e_phentsize;

   shdr_mioff    = ehdr_mioff + ehdr_m.e_shoff;
   shdr_mnent    = ehdr_m.e_shnum;
   shdr_ment_szB = ehdr_m.e_shentsize;

   TRACE_SYMTAB("------ Basic facts about the object ------\n");
   TRACE_SYMTAB("object:  n_oimage %llu\n",
                (ULong)ML_(img_size)(mimg));
   TRACE_SYMTAB("phdr:    ioff %llu nent %ld ent_szB %ld\n",
               phdr_mioff, phdr_mnent, phdr_ment_szB);
   TRACE_SYMTAB("shdr:    ioff %llu nent %ld ent_szB %ld\n",
               shdr_mioff, shdr_mnent, shdr_ment_szB);
   for (i = 0; i < VG_(sizeXA)(di->fsm.maps); i++) {
      struct _DebugInfoMapping* map = VG_(indexXA)(di->fsm.maps, i);
      if (map->rx)
         TRACE_SYMTAB("rx_map:  avma %#lx   size %lu  foff %lu\n",
                      map->avma, map->size, map->foff);
   }
   for (i = 0; i < VG_(sizeXA)(di->fsm.maps); i++) {
      struct _DebugInfoMapping* map = VG_(indexXA)(di->fsm.maps, i);
      if (map->rw)
         TRACE_SYMTAB("rw_map:  avma %#lx   size %lu  foff %lu\n",
                      map->avma, map->size, map->foff);
   }

   if (phdr_mnent == 0
       || !ML_(img_valid)(mimg, phdr_mioff, phdr_mnent * phdr_ment_szB)) {
      ML_(symerr)(di, True, "Missing or invalid ELF Program Header Table");
      goto out;
   }

   if (shdr_mnent == 0
       || !ML_(img_valid)(mimg, shdr_mioff, shdr_mnent * shdr_ment_szB)) {
      ML_(symerr)(di, True, "Missing or invalid ELF Section Header Table");
      goto out;
   }

   /* Also find the section header's string table, and validate. */
   /* checked previously by is_elf_object_file: */
   vg_assert(ehdr_m.e_shstrndx != SHN_UNDEF);

   // shdr_mioff is the offset of the section header table
   // and we need the ehdr_m.e_shstrndx'th entry
   { ElfXX_Shdr a_shdr;
     ML_(img_get)(&a_shdr, mimg,
                  INDEX_BIS(shdr_mioff, ehdr_m.e_shstrndx, shdr_ment_szB),
                  sizeof(a_shdr));
     shdr_strtab_mioff
        = ehdr_mioff /* isn't this always zero? */ + a_shdr.sh_offset;

     if (!ML_(img_valid)(mimg, shdr_strtab_mioff, 
                         1/*bogus, but we don't know the real size*/ )) {
        ML_(symerr)(di, True, "Invalid ELF Section Header String Table");
        goto out;
     }
   }

   TRACE_SYMTAB("shdr:    string table at %llu\n", shdr_strtab_mioff);

   svma_ranges = VG_(newXA)(ML_(dinfo_zalloc), "di.relfdi.1",
                            ML_(dinfo_free), sizeof(RangeAndBias));

   /* TOPLEVEL */
   /* Look through the program header table, and:
      - copy information from suitable PT_LOAD entries into svma_ranges
      - find (or fake up) the .soname for this object.
   */
   TRACE_SYMTAB("\n");
   TRACE_SYMTAB("------ Examining the program headers ------\n");
   vg_assert(di->soname == NULL);
   {
      /* TOPLEVEL */
      ElfXX_Addr prev_svma = 0;

      for (i = 0; i < phdr_mnent; i++) {
         ElfXX_Phdr a_phdr;
         ML_(img_get)(&a_phdr, mimg,
                      INDEX_BIS(phdr_mioff, i, phdr_ment_szB),
                      sizeof(a_phdr));

         /* Make sure the PT_LOADable entries are in order and
            non-overlapping.  This in turn means the address ranges
            slurped into svma_ranges are in order and
            non-overlapping. */

         if (a_phdr.p_type == PT_LOAD) {
            TRACE_SYMTAB("PT_LOAD[%ld]: p_vaddr %#lx (prev %#lx)\n",
                         i, (UWord)a_phdr.p_vaddr, (UWord)prev_svma);
            TRACE_SYMTAB("PT_LOAD[%ld]:   p_offset %lu, p_filesz %lu,"
                         " perms %c%c%c\n",
                         i, (UWord)a_phdr.p_offset, (UWord)a_phdr.p_filesz,
                         a_phdr.p_flags & PF_R ? 'r' : '-',
                         a_phdr.p_flags & PF_W ? 'w' : '-',
                         a_phdr.p_flags & PF_X ? 'x' : '-');
            if (a_phdr.p_vaddr < prev_svma) {
               ML_(symerr)(di, True,
                           "ELF Program Headers are not in ascending order");
               goto out;
            }
            prev_svma = a_phdr.p_vaddr;
            if (a_phdr.p_memsz > 0) {
               Bool loaded = False;
               for (j = 0; j < VG_(sizeXA)(di->fsm.maps); j++) {
                  struct _DebugInfoMapping* map = VG_(indexXA)(di->fsm.maps, j);
                  if (   (map->rx || map->rw)
                      && a_phdr.p_offset >= map->foff
                      && a_phdr.p_offset <  map->foff + map->size
                      && a_phdr.p_offset + a_phdr.p_filesz 
                         <= map->foff + map->size) {
                     RangeAndBias item;
                     item.svma_base  = a_phdr.p_vaddr;
                     item.svma_limit = a_phdr.p_vaddr + a_phdr.p_memsz;
                     item.bias       = map->avma - map->foff
                                       + a_phdr.p_offset - a_phdr.p_vaddr;
                     if (map->rw
                         && (a_phdr.p_flags & (PF_R | PF_W))
                            == (PF_R | PF_W)) {
                        item.exec = False;
                        VG_(addToXA)(svma_ranges, &item);
                        TRACE_SYMTAB("PT_LOAD[%ld]:   acquired as rw\n", i);
                        loaded = True;
                     }
                     if (map->rx
                         && (a_phdr.p_flags & (PF_R | PF_X))
                            == (PF_R | PF_X)) {
                        item.exec = True;
                        VG_(addToXA)(svma_ranges, &item);
                        TRACE_SYMTAB("PT_LOAD[%ld]:   acquired as rx\n", i);
                        loaded = True;
                     }
                  }
               }
               if (!loaded) {
                  ML_(symerr)(di, False,
                              "ELF section outside all mapped regions");
                  /* This problem might be solved by further memory mappings.
                     Avoid the vg_assert(!di->soname) at the beginning of this
                     function if DYNAMIC section has been already processed. */
                  if (di->soname) {
                     ML_(dinfo_free)(di->soname);
                     di->soname = NULL;
                  }
                  goto out;
               }
            }
         }

         /* Try to get the soname.  If there isn't one, use "NONE".
            The seginfo needs to have some kind of soname in order to
            facilitate writing redirect functions, since all redirect
            specifications require a soname (pattern). */
         if (a_phdr.p_type == PT_DYNAMIC && di->soname == NULL) {
            Word   stroff       = -1;
            DiOffT strtab_mioff = DiOffT_INVALID;
            for (j = 0; True/*exit check is in the loop*/; j++) {
               ElfXX_Dyn t_dyn_m; /* dyn_img[j] */
               ML_(img_get)(&t_dyn_m, mimg,
                            INDEX_BIS(ehdr_mioff + a_phdr.p_offset,
                                      j, sizeof(ElfXX_Dyn)),
                            sizeof(t_dyn_m));
               if (t_dyn_m.d_tag == DT_NULL)
                  break;

               switch (t_dyn_m.d_tag) {
                  case DT_SONAME: {
                     stroff = t_dyn_m.d_un.d_val;
                     break;
                  }
                  case DT_STRTAB: {
                     Bool ok2 = False;
                     Word offset = file_offset_from_svma(
                                      &ok2, t_dyn_m.d_un.d_ptr, mimg,
                                      phdr_mioff, phdr_mnent, phdr_ment_szB
                                   );
                     if (ok2 && strtab_mioff == DiOffT_INVALID) {
                        // Check for obviously bogus offsets.
                        if (!ML_(img_valid)(mimg, offset, 1)) {
                           ML_(symerr)(di, True, "Invalid DT_STRTAB offset");
                           goto out;
                        }
                        strtab_mioff = ehdr_mioff + offset;
                        vg_assert(ehdr_mioff == 0); // should always be
                     }
                     break;
                  }
                  default:
                     break;
               }
            }
            if (stroff != -1 && strtab_mioff != DiOffT_INVALID) {
               di->soname = ML_(img_strdup)(mimg, "di.redi.1",
                                            strtab_mioff + stroff);
               TRACE_SYMTAB("Found soname = %s\n", di->soname);
            }
         }
      } /* for (i = 0; i < phdr_Mnent; i++) ... */
      /* TOPLEVEL */

   } /* examine the program headers (local scope) */

   /* TOPLEVEL */

   /* If, after looking at all the program headers, we still didn't 
      find a soname, add a fake one. */
   if (di->soname == NULL) {
      TRACE_SYMTAB("No soname found; using (fake) \"NONE\"\n");
      di->soname = ML_(dinfo_strdup)("di.redi.2", "NONE");
   }

   vg_assert(VG_(sizeXA)(svma_ranges) != 0);

   /* Now read the section table. */
   TRACE_SYMTAB("\n");
   TRACE_SYMTAB("------ Examining the section headers ------\n");
   for (i = 0; i < VG_(sizeXA)(di->fsm.maps); i++) {
      struct _DebugInfoMapping* map = VG_(indexXA)(di->fsm.maps, i);
      if (map->rx)
         TRACE_SYMTAB("rx: at %#lx are mapped foffsets %ld .. %ld\n",
                      map->avma, map->foff, map->foff + map->size - 1 );
   }
   TRACE_SYMTAB("rx: contains these svma regions:\n");
   for (i = 0; i < VG_(sizeXA)(svma_ranges); i++) {
      RangeAndBias* reg = VG_(indexXA)(svma_ranges, i);
      if (reg->exec)
         TRACE_SYMTAB("  svmas %#lx .. %#lx with bias %#lx\n",
                      reg->svma_base, reg->svma_limit - 1, reg->bias );
   }
   for (i = 0; i < VG_(sizeXA)(di->fsm.maps); i++) {
      struct _DebugInfoMapping* map = VG_(indexXA)(di->fsm.maps, i);
      if (map->rw)
         TRACE_SYMTAB("rw: at %#lx are mapped foffsets %ld .. %ld\n",
                      map->avma, map->foff, map->foff + map->size - 1 );
   }
   TRACE_SYMTAB("rw: contains these svma regions:\n");
   for (i = 0; i < VG_(sizeXA)(svma_ranges); i++) {
      RangeAndBias* reg = VG_(indexXA)(svma_ranges, i);
      if (!reg->exec)
         TRACE_SYMTAB("  svmas %#lx .. %#lx with bias %#lx\n",
                      reg->svma_base, reg->svma_limit - 1, reg->bias );
   }

   /* TOPLEVEL */
   /* Iterate over section headers */
   for (i = 0; i < shdr_mnent; i++) {
      ElfXX_Shdr a_shdr;
      ML_(img_get)(&a_shdr, mimg,
                   INDEX_BIS(shdr_mioff, i, shdr_ment_szB), sizeof(a_shdr));
      DiOffT name_mioff = shdr_strtab_mioff + a_shdr.sh_name;
      HChar* name = ML_(img_strdup)(mimg, "di.redi_name.1", name_mioff);
      Addr   svma = a_shdr.sh_addr;
      OffT   foff = a_shdr.sh_offset;
      UWord  size = a_shdr.sh_size; /* Do not change this to be signed. */
      UInt   alyn = a_shdr.sh_addralign;
      Bool   bits = !(a_shdr.sh_type == SHT_NOBITS);
      /* Look through our collection of info obtained from the PT_LOAD
         headers, and make 'inrx' and 'inrw' point to the first entry
         in each that intersects 'avma'.  If in each case none is found,
         leave the relevant pointer at NULL. */
      RangeAndBias* inrx = NULL;
      RangeAndBias* inrw = NULL;
      for (j = 0; j < VG_(sizeXA)(svma_ranges); j++) {
         RangeAndBias* rng = VG_(indexXA)(svma_ranges, j);
         if (svma >= rng->svma_base && svma < rng->svma_limit) {
            if (!inrx && rng->exec) {
               inrx = rng;
            } else if (!inrw && !rng->exec) {
               inrw = rng;
            }
            if (inrx && inrw)
               break;
         }
      }

      TRACE_SYMTAB(" [sec %2ld]  %s %s  al%2u  foff %6ld .. %6ld  "
                   "  svma %p  name \"%s\"\n", 
                   i, inrx ? "rx" : "  ", inrw ? "rw" : "  ", alyn,
                   foff, foff+size-1, (void*)svma, name);

      /* Check for sane-sized segments.  SHT_NOBITS sections have zero
         size in the file. */
      if ((foff >= ML_(img_size)(mimg)) 
          || (foff + (bits ? size : 0) > ML_(img_size)(mimg))) {
         ML_(symerr)(di, True, "ELF Section extends beyond image end");
         goto out;
      }

      /* Check for a sane alignment value. */
      if (alyn > 0 && -1 == VG_(log2)(alyn)) {
         ML_(symerr)(di, True, "ELF Section contains invalid "
                               ".sh_addralign value");
         goto out;
      }

      /* Ignore zero sized sections. */
      if (size == 0) {
         TRACE_SYMTAB("zero sized section \"%s\", ignoring\n", name);
         ML_(dinfo_free)(name);
         continue;
      }

#     define BAD(_secname)                                 \
         do { ML_(symerr)(di, True,                        \
                          "Can't make sense of " _secname  \
                          " section mapping");             \
              /* make sure we don't assert if we find */   \
              /* ourselves back in this routine later, */  \
              /* with the same di */                       \
              di->soname = NULL;                           \
              goto out;                                    \
         } while (0)

      /* Find avma-s for: .text .data .sdata .rodata .bss .sbss .plt .got .opd
         and .eh_frame */

      /* Accept .text where mapped as rx (code), even if zero-sized */
      if (0 == VG_(strcmp)(name, ".text")) {
         if (inrx && !di->text_present) {
            di->text_present = True;
            di->text_svma = svma;
            di->text_avma = svma + inrx->bias;
            di->text_size = size;
            di->text_bias = inrx->bias;
            di->text_debug_svma = svma;
            di->text_debug_bias = inrx->bias;
            TRACE_SYMTAB("acquiring .text svma = %#lx .. %#lx\n",
                         di->text_svma, 
                         di->text_svma + di->text_size - 1);
            TRACE_SYMTAB("acquiring .text avma = %#lx .. %#lx\n",
                         di->text_avma, 
                         di->text_avma + di->text_size - 1);
            TRACE_SYMTAB("acquiring .text bias = %#lx\n", di->text_bias);
         } else {
            BAD(".text");
         }
      }

      /* Accept .data where mapped as rw (data), even if zero-sized */
      if (0 == VG_(strcmp)(name, ".data")) {
         if (inrw && !di->data_present) {
            di->data_present = True;
            di->data_svma = svma;
            di->data_avma = svma + inrw->bias;
            di->data_size = size;
            di->data_bias = inrw->bias;
            di->data_debug_svma = svma;
            di->data_debug_bias = inrw->bias;
            TRACE_SYMTAB("acquiring .data svma = %#lx .. %#lx\n",
                         di->data_svma,
                         di->data_svma + di->data_size - 1);
            TRACE_SYMTAB("acquiring .data avma = %#lx .. %#lx\n",
                         di->data_avma,
                         di->data_avma + di->data_size - 1);
            TRACE_SYMTAB("acquiring .data bias = %#lx\n", di->data_bias);
         } else {
            BAD(".data");
         }
      }

      /* Accept .sdata where mapped as rw (data) */
      if (0 == VG_(strcmp)(name, ".sdata")) {
         if (inrw && !di->sdata_present) {
            di->sdata_present = True;
            di->sdata_svma = svma;
            di->sdata_avma = svma + inrw->bias;
            di->sdata_size = size;
            di->sdata_bias = inrw->bias;
            di->sdata_debug_svma = svma;
            di->sdata_debug_bias = inrw->bias;
            TRACE_SYMTAB("acquiring .sdata svma = %#lx .. %#lx\n",
                         di->sdata_svma,
                         di->sdata_svma + di->sdata_size - 1);
            TRACE_SYMTAB("acquiring .sdata avma = %#lx .. %#lx\n",
                         di->sdata_avma,
                         di->sdata_avma + di->sdata_size - 1);
            TRACE_SYMTAB("acquiring .sdata bias = %#lx\n", di->sdata_bias);
         } else {
            BAD(".sdata");
         }
      }

      /* Accept .rodata where mapped as rx (data), even if zero-sized */
      if (0 == VG_(strcmp)(name, ".rodata")) {
         if (inrx && !di->rodata_present) {
            di->rodata_present = True;
            di->rodata_svma = svma;
            di->rodata_avma = svma + inrx->bias;
            di->rodata_size = size;
            di->rodata_bias = inrx->bias;
            di->rodata_debug_svma = svma;
            di->rodata_debug_bias = inrx->bias;
                                    /* NB was 'inrw' prior to r11794 */
            TRACE_SYMTAB("acquiring .rodata svma = %#lx .. %#lx\n",
                         di->rodata_svma,
                         di->rodata_svma + di->rodata_size - 1);
            TRACE_SYMTAB("acquiring .rodata avma = %#lx .. %#lx\n",
                         di->rodata_avma,
                         di->rodata_avma + di->rodata_size - 1);
            TRACE_SYMTAB("acquiring .rodata bias = %#lx\n", di->rodata_bias);
         } else {
            BAD(".rodata");
         }
      }

      if (0 == VG_(strcmp)(name, ".dynbss")) {
         if (inrw && !di->bss_present) {
            dynbss_present = True;
            di->bss_present = True;
            di->bss_svma = svma;
            di->bss_avma = svma + inrw->bias;
            di->bss_size = size;
            di->bss_bias = inrw->bias;
            di->bss_debug_svma = svma;
            di->bss_debug_bias = inrw->bias;
            TRACE_SYMTAB("acquiring .dynbss svma = %#lx .. %#lx\n",
                         di->bss_svma,
                         di->bss_svma + di->bss_size - 1);
            TRACE_SYMTAB("acquiring .dynbss avma = %#lx .. %#lx\n",
                         di->bss_avma,
                         di->bss_avma + di->bss_size - 1);
            TRACE_SYMTAB("acquiring .dynbss bias = %#lx\n", di->bss_bias);
         }
      }

      /* Accept .bss where mapped as rw (data), even if zero-sized */
      if (0 == VG_(strcmp)(name, ".bss")) {
         if (inrw && dynbss_present) {
            vg_assert(di->bss_present);
            dynbss_present = False;
            vg_assert(di->bss_svma + di->bss_size == svma);
            di->bss_size += size;
            TRACE_SYMTAB("acquiring .bss svma = %#lx .. %#lx\n",
                         svma, svma + size - 1);
            TRACE_SYMTAB("acquiring .bss avma = %#lx .. %#lx\n",
                         svma + inrw->bias, svma + inrw->bias + size - 1);
            TRACE_SYMTAB("acquiring .bss bias = %#lx\n", di->bss_bias);
         } else

         if (inrw && !di->bss_present) {
            di->bss_present = True;
            di->bss_svma = svma;
            di->bss_avma = svma + inrw->bias;
            di->bss_size = size;
            di->bss_bias = inrw->bias;
            di->bss_debug_svma = svma;
            di->bss_debug_bias = inrw->bias;
            TRACE_SYMTAB("acquiring .bss svma = %#lx .. %#lx\n",
                         di->bss_svma,
                         di->bss_svma + di->bss_size - 1);
            TRACE_SYMTAB("acquiring .bss avma = %#lx .. %#lx\n",
                         di->bss_avma,
                         di->bss_avma + di->bss_size - 1);
            TRACE_SYMTAB("acquiring .bss bias = %#lx\n", di->bss_bias);
         } else

         /* Now one from the wtf?! department ... */
         if (inrx && (!inrw) && !di->bss_present) {
            /* File contains a .bss, but it got mapped as rx only.
               This is very strange.  For now, just pretend we didn't
               see it :-) */
            di->bss_present = False;
            di->bss_svma = 0;
            di->bss_avma = 0;
            di->bss_size = 0;
            di->bss_bias = 0;
            di->bss_debug_svma = 0;
            di->bss_debug_bias = 0;
            if (!VG_(clo_xml)) {
               VG_(message)(Vg_UserMsg,
                            "Warning: the following file's .bss is "
                            "mapped r-x only - ignoring .bss syms\n");
               VG_(message)(Vg_UserMsg,   " %s\n", di->fsm.filename 
                                                      ? di->fsm.filename
                                                      : "(null?!)" );
            }
         } else

         if ((!inrw) && (!inrx) && !di->bss_present) {
            /* File contains a .bss, but it didn't get mapped.  Ignore. */
            di->bss_present = False;
            di->bss_svma = 0;
            di->bss_avma = 0;
            di->bss_size = 0;
            di->bss_bias = 0;
         } else {
            BAD(".bss");
         }
      }

      if (0 == VG_(strcmp)(name, ".sdynbss")) {
         if (inrw && !di->sbss_present) {
            sdynbss_present = True;
            di->sbss_present = True;
            di->sbss_svma = svma;
            di->sbss_avma = svma + inrw->bias;
            di->sbss_size = size;
            di->sbss_bias = inrw->bias;
            di->sbss_debug_svma = svma;
            di->sbss_debug_bias = inrw->bias;
            TRACE_SYMTAB("acquiring .sdynbss svma = %#lx .. %#lx\n",
                         di->sbss_svma,
                         di->sbss_svma + di->sbss_size - 1);
            TRACE_SYMTAB("acquiring .sdynbss avma = %#lx .. %#lx\n",
                         di->sbss_avma,
                         di->sbss_avma + di->sbss_size - 1);
            TRACE_SYMTAB("acquiring .sdynbss bias = %#lx\n", di->sbss_bias);
         }
      }

      /* Accept .sbss where mapped as rw (data) */
      if (0 == VG_(strcmp)(name, ".sbss")) {
         if (inrw && sdynbss_present) {
            vg_assert(di->sbss_present);
            sdynbss_present = False;
            vg_assert(di->sbss_svma + di->sbss_size == svma);
            di->sbss_size += size;
            TRACE_SYMTAB("acquiring .sbss svma = %#lx .. %#lx\n",
                         svma, svma + size - 1);
            TRACE_SYMTAB("acquiring .sbss avma = %#lx .. %#lx\n",
                         svma + inrw->bias, svma + inrw->bias + size - 1);
            TRACE_SYMTAB("acquiring .sbss bias = %#lx\n", di->sbss_bias);
         } else

         if (inrw && !di->sbss_present) {
            di->sbss_present = True;
            di->sbss_svma = svma;
            di->sbss_avma = svma + inrw->bias;
            di->sbss_size = size;
            di->sbss_bias = inrw->bias;
            di->sbss_debug_svma = svma;
            di->sbss_debug_bias = inrw->bias;
            TRACE_SYMTAB("acquiring .sbss svma = %#lx .. %#lx\n",
                         di->sbss_svma,
                         di->sbss_svma + di->sbss_size - 1);
            TRACE_SYMTAB("acquiring .sbss avma = %#lx .. %#lx\n",
                         di->sbss_avma,
                         di->sbss_avma + di->sbss_size - 1);
            TRACE_SYMTAB("acquiring .sbss bias = %#lx\n", di->sbss_bias);
         } else {
            BAD(".sbss");
         }
      }

      /* Accept .got where mapped as rw (data) */
      if (0 == VG_(strcmp)(name, ".got")) {
         if (inrw && !di->got_present) {
            di->got_present = True;
            di->got_avma = svma + inrw->bias;
            di->got_size = size;
            TRACE_SYMTAB("acquiring .got avma = %#lx\n", di->got_avma);
         } else {
            BAD(".got");
         }
      }

      /* Accept .got.plt where mapped as rw (data) */
      if (0 == VG_(strcmp)(name, ".got.plt")) {
         if (inrw && !di->gotplt_present) {
            di->gotplt_present = True;
            di->gotplt_avma = svma + inrw->bias;
            di->gotplt_size = size;
            TRACE_SYMTAB("acquiring .got.plt avma = %#lx\n", di->gotplt_avma);
         } else if (size != 0) {
            BAD(".got.plt");
         }
      }

      /* PLT is different on different platforms, it seems. */
#     if defined(VGP_x86_linux) || defined(VGP_amd64_linux) \
         || defined(VGP_arm_linux) || defined (VGP_s390x_linux) \
         || defined(VGP_mips32_linux) || defined(VGP_mips64_linux)
      /* Accept .plt where mapped as rx (code) */
      if (0 == VG_(strcmp)(name, ".plt")) {
         if (inrx && !di->plt_present) {
            di->plt_present = True;
            di->plt_avma = svma + inrx->bias;
            di->plt_size = size;
            TRACE_SYMTAB("acquiring .plt avma = %#lx\n", di->plt_avma);
         } else {
            BAD(".plt");
         }
      }
#     elif defined(VGP_ppc32_linux)
      /* Accept .plt where mapped as rw (data) */
      if (0 == VG_(strcmp)(name, ".plt")) {
         if (inrw && !di->plt_present) {
            di->plt_present = True;
            di->plt_avma = svma + inrw->bias;
            di->plt_size = size;
            TRACE_SYMTAB("acquiring .plt avma = %#lx\n", di->plt_avma);
         } else {
            BAD(".plt");
         }
      }
#     elif defined(VGP_ppc64_linux)
      /* Accept .plt where mapped as rw (data), or unmapped */
      if (0 == VG_(strcmp)(name, ".plt")) {
         if (inrw && !di->plt_present) {
            di->plt_present = True;
            di->plt_avma = svma + inrw->bias;
            di->plt_size = size;
            TRACE_SYMTAB("acquiring .plt avma = %#lx\n", di->plt_avma);
         } else 
         if ((!inrw) && (!inrx) && size > 0 && !di->plt_present) {
            /* File contains a .plt, but it didn't get mapped.
               Presumably it is not required on this platform.  At
               least don't reject the situation as invalid. */
            di->plt_present = True;
            di->plt_avma = 0;
            di->plt_size = 0;
         } else {
            BAD(".plt");
         }
      }
#     else
#       error "Unsupported platform"
#     endif

      /* Accept .opd where mapped as rw (data) */
      if (0 == VG_(strcmp)(name, ".opd")) {
         if (inrw && !di->opd_present) {
            di->opd_present = True;
            di->opd_avma = svma + inrw->bias;
            di->opd_size = size;
            TRACE_SYMTAB("acquiring .opd avma = %#lx\n", di->opd_avma);
         } else {
            BAD(".opd");
         }
      }

      /* Accept .eh_frame where mapped as rx (code).  This seems to be
         the common case.  However, if that doesn't pan out, try for
         rw (data) instead.  We can handle up to N_EHFRAME_SECTS per
         ELF object. */
      if (0 == VG_(strcmp)(name, ".eh_frame")) {
         if (inrx && di->n_ehframe < N_EHFRAME_SECTS) {
            di->ehframe_avma[di->n_ehframe] = svma + inrx->bias;
            di->ehframe_size[di->n_ehframe] = size;
            TRACE_SYMTAB("acquiring .eh_frame avma = %#lx\n",
                         di->ehframe_avma[di->n_ehframe]);
            di->n_ehframe++;
         } else
         if (inrw && di->n_ehframe < N_EHFRAME_SECTS) {
            di->ehframe_avma[di->n_ehframe] = svma + inrw->bias;
            di->ehframe_size[di->n_ehframe] = size;
            TRACE_SYMTAB("acquiring .eh_frame avma = %#lx\n",
                         di->ehframe_avma[di->n_ehframe]);
            di->n_ehframe++;
         } else {
            BAD(".eh_frame");
         }
      }

      ML_(dinfo_free)(name);

#     undef BAD

   } /* iterate over the section headers */

   /* TOPLEVEL */
   if (0) VG_(printf)("YYYY text_: avma %#lx  size %ld  bias %#lx\n",
                      di->text_avma, di->text_size, di->text_bias);

   if (VG_(clo_verbosity) > 2 || VG_(clo_trace_redir))
      VG_(message)(Vg_DebugMsg, "   svma %#010lx, avma %#010lx\n",
                                di->text_avma - di->text_bias,
                                di->text_avma );

   TRACE_SYMTAB("\n");
   TRACE_SYMTAB("------ Finding image addresses "
                "for debug-info sections ------\n");

   /* TOPLEVEL */
   /* Find interesting sections, read the symbol table(s), read any
      debug information.  Each section is located either in the main,
      debug or alt-debug files, but only in one.  For each section,
      |section_escn| records which of |mimg|, |dimg| or |aimg| we
      found it in, along with the section's image offset and its size.
      The triples (section_img, section_ioff, section_szB) are
      consistent, in that they are always either (NULL,
      DiOffT_INVALID, 0), or refer to the same image, and are all
      assigned together. */
   {
      /* TOPLEVEL */
      DiSlice strtab_escn         = DiSlice_INVALID; // .strtab
      DiSlice symtab_escn         = DiSlice_INVALID; // .symtab
      DiSlice dynstr_escn         = DiSlice_INVALID; // .dynstr
      DiSlice dynsym_escn         = DiSlice_INVALID; // .dynsym
      DiSlice debuglink_escn      = DiSlice_INVALID; // .gnu_debuglink
      DiSlice debugaltlink_escn   = DiSlice_INVALID; // .gnu_debugaltlink
      DiSlice stab_escn           = DiSlice_INVALID; // .stab         (stabs) 
      DiSlice stabstr_escn        = DiSlice_INVALID; // .stabstr      (stabs) 
      DiSlice debug_line_escn     = DiSlice_INVALID; // .debug_line   (dwarf2)
      DiSlice debug_info_escn     = DiSlice_INVALID; // .debug_info   (dwarf2)
      DiSlice debug_types_escn    = DiSlice_INVALID; // .debug_types  (dwarf4)
      DiSlice debug_abbv_escn     = DiSlice_INVALID; // .debug_abbrev (dwarf2)
      DiSlice debug_str_escn      = DiSlice_INVALID; // .debug_str    (dwarf2)
      DiSlice debug_ranges_escn   = DiSlice_INVALID; // .debug_ranges (dwarf2)
      DiSlice debug_loc_escn      = DiSlice_INVALID; // .debug_loc    (dwarf2)
      DiSlice debug_frame_escn    = DiSlice_INVALID; // .debug_frame  (dwarf2)
      DiSlice debug_line_alt_escn = DiSlice_INVALID; // .debug_line   (alt)
      DiSlice debug_info_alt_escn = DiSlice_INVALID; // .debug_info   (alt)
      DiSlice debug_abbv_alt_escn = DiSlice_INVALID; // .debug_abbrev (alt)
      DiSlice debug_str_alt_escn  = DiSlice_INVALID; // .debug_str    (alt)
      DiSlice dwarf1d_escn        = DiSlice_INVALID; // .debug        (dwarf1)
      DiSlice dwarf1l_escn        = DiSlice_INVALID; // .line         (dwarf1)
      DiSlice opd_escn            = DiSlice_INVALID; // .opd (dwarf2, 
                                                     //       ppc64-linux)
      DiSlice ehframe_escn[N_EHFRAME_SECTS];         // .eh_frame (dwarf2)

      for (i = 0; i < N_EHFRAME_SECTS; i++)
         ehframe_escn[i] = DiSlice_INVALID;

      /* Find all interesting sections */

      UInt ehframe_mix = 0;

      /* What FIND does: it finds the section called _SEC_NAME.  The
         size of it is assigned to _SEC_SIZE.  The address of the
         section in the transiently loaded oimage is assigned to
         _SEC_IMG.  If the section is found, _POST_FX is executed
         after _SEC_NAME and _SEC_SIZE have been assigned to.

         Even for sections which are marked loadable, the client's
         ld.so may not have loaded them yet, so there is no guarantee
         that we can safely prod around in any such area).  Because
         the entire object file is transiently mapped aboard for
         inspection, it's always safe to inspect that area. */

      /* TOPLEVEL */
      /* Iterate over section headers (again) */
      for (i = 0; i < ehdr_m.e_shnum; i++) {

#        define FINDX(_sec_name, _sec_escn, _post_fx) \
         do { \
            ElfXX_Shdr a_shdr; \
            ML_(img_get)(&a_shdr, mimg, \
                         INDEX_BIS(shdr_mioff, i, shdr_ment_szB), \
                         sizeof(a_shdr)); \
            if (0 == ML_(img_strcmp_c)(mimg, shdr_strtab_mioff \
                                              + a_shdr.sh_name, _sec_name)) { \
               Bool nobits; \
               _sec_escn.img  = mimg; \
               _sec_escn.ioff = (DiOffT)a_shdr.sh_offset; \
               _sec_escn.szB  = a_shdr.sh_size; \
               nobits         = a_shdr.sh_type == SHT_NOBITS; \
               vg_assert(_sec_escn.img  != NULL); \
               vg_assert(_sec_escn.ioff != DiOffT_INVALID); \
               TRACE_SYMTAB( "%18s:  ioff %llu .. %llu\n", \
                             _sec_name, (ULong)_sec_escn.ioff, \
                             ((ULong)_sec_escn.ioff) + _sec_escn.szB - 1); \
               /* SHT_NOBITS sections have zero size in the file. */ \
               if ( a_shdr.sh_offset \
                    + (nobits ? 0 : _sec_escn.szB) > ML_(img_size)(mimg) ) { \
                  ML_(symerr)(di, True, \
                              "   section beyond image end?!"); \
                  goto out; \
               } \
               _post_fx; \
            } \
         } while (0);

         /* Version with no post-effects */
#        define FIND(_sec_name, _sec_escn) \
            FINDX(_sec_name, _sec_escn, /**/)

         /*   NAME                  ElfSec */
         FIND(".dynsym",            dynsym_escn)
         FIND(".dynstr",            dynstr_escn)
         FIND(".symtab",            symtab_escn)
         FIND(".strtab",            strtab_escn)

         FIND(".gnu_debuglink",     debuglink_escn)
         FIND(".gnu_debugaltlink",  debugaltlink_escn)

         FIND(".stab",              stab_escn)
         FIND(".stabstr",           stabstr_escn)

         FIND(".debug_line",        debug_line_escn)
         FIND(".debug_info",        debug_info_escn)
         FIND(".debug_types",       debug_types_escn)
         FIND(".debug_abbrev",      debug_abbv_escn)
         FIND(".debug_str",         debug_str_escn)
         FIND(".debug_ranges",      debug_ranges_escn)
         FIND(".debug_loc",         debug_loc_escn)
         FIND(".debug_frame",       debug_frame_escn)

         FIND(".debug",             dwarf1d_escn)
         FIND(".line",              dwarf1l_escn)

         FIND(".opd",               opd_escn)

         FINDX(".eh_frame",         ehframe_escn[ehframe_mix],
               do { ehframe_mix++; vg_assert(ehframe_mix <= N_EHFRAME_SECTS);
               } while (0)
         )
         /* Comment_on_EH_FRAME_MULTIPLE_INSTANCES: w.r.t. .eh_frame
            multi-instance kludgery, how are we assured that the order
            in which we fill in ehframe_escn[] is consistent with the
            order in which we previously filled in di->ehframe_avma[]
            and di->ehframe_size[] ?  By the fact that in both cases,
            these arrays were filled in by iterating over the section
            headers top-to-bottom.  So both loops (this one and the
            previous one) encounter the .eh_frame entries in the same
            order and so fill in these arrays in a consistent order.
         */

#        undef FINDX
#        undef FIND
      } /* Iterate over section headers (again) */

      /* TOPLEVEL */
      /* Now, see if we can find a debuginfo object, and if so connect
         |dimg| to it. */
      vg_assert(dimg == NULL && aimg == NULL);

      /* Look for a build-id */
      HChar* buildid = find_buildid(mimg, False, False);

      /* Look for a debug image that matches either the build-id or
         the debuglink-CRC32 in the main image.  If the main image
         doesn't contain either of those then this won't even bother
         to try looking.  This looks in all known places, including
         the --extra-debuginfo-path if specified and on the
         --debuginfo-server if specified. */
      if (buildid != NULL || debuglink_escn.img != NULL) {
         /* Do have a debuglink section? */
         if (debuglink_escn.img != NULL) {
            UInt crc_offset 
               = VG_ROUNDUP(ML_(img_strlen)(debuglink_escn.img,
                                            debuglink_escn.ioff)+1, 4);
            vg_assert(crc_offset + sizeof(UInt) <= debuglink_escn.szB);

            /* Extract the CRC from the debuglink section */
            UInt crc = ML_(img_get_UInt)(debuglink_escn.img,
                                         debuglink_escn.ioff + crc_offset);

            /* See if we can find a matching debug file */
            HChar* debuglink_str_m
               = ML_(img_strdup)(debuglink_escn.img,
                                 "di.redi_dlk.1", debuglink_escn.ioff);
            dimg = find_debug_file( di, di->fsm.filename, buildid,
                                    debuglink_str_m, crc, False );
            if (debuglink_str_m)
               ML_(dinfo_free)(debuglink_str_m);
         } else {
            /* See if we can find a matching debug file */
            dimg = find_debug_file( di, di->fsm.filename, buildid,
                                    NULL, 0, False );
         }
      }

      if (buildid) {
         ML_(dinfo_free)(buildid);
         buildid = NULL; /* paranoia */
      }

      /* As a last-ditch measure, try looking for in the
         --extra-debuginfo-path and/or on the --debuginfo-server, but
         only in the case where --allow-mismatched-debuginfo=yes.
         This is dangerous in that (1) it gives no assurance that the
         debuginfo object matches the main one, and hence (2) we will
         very likely get an assertion in the code below, if indeed
         there is a mismatch.  Hence it is disabled by default
         (--allow-mismatched-debuginfo=no).  Nevertheless it's
         sometimes a useful way of getting out of a tight spot.

         Note that we're ignoring the name in the .gnu_debuglink
         section here, and just looking for a file of the same name
         either the extra-path or on the server. */
      if (dimg == NULL && VG_(clo_allow_mismatched_debuginfo)) {
         dimg = find_debug_file_ad_hoc( di, di->fsm.filename );
      }

      /* TOPLEVEL */
      /* If we were successful in finding a debug image, pull various
         SVMA/bias/size and image addresses out of it. */
      if (dimg != NULL && is_elf_object_file_by_DiImage(dimg, False)) {

         /* Pull out and validate program header and section header info */
         DiOffT      ehdr_dioff = 0;
         ElfXX_Ehdr  ehdr_dimg;
         ML_(img_get)(&ehdr_dimg, dimg, ehdr_dioff, sizeof(ehdr_dimg));

         DiOffT   phdr_dioff        = ehdr_dimg.e_phoff;
         UWord    phdr_dnent        = ehdr_dimg.e_phnum;
         UWord    phdr_dent_szB     = ehdr_dimg.e_phentsize;

         DiOffT   shdr_dioff        = ehdr_dimg.e_shoff;
         UWord    shdr_dnent        = ehdr_dimg.e_shnum;
         UWord    shdr_dent_szB     = ehdr_dimg.e_shentsize;

         DiOffT   shdr_strtab_dioff = DiOffT_INVALID;

         /* SVMAs covered by rx and rw segments and corresponding bias. */
         Addr     rx_dsvma_limit = 0;
         PtrdiffT rx_dbias = 0;
         Addr     rw_dsvma_limit = 0;
         PtrdiffT rw_dbias = 0;

         Bool need_symtab, need_stabs, need_dwarf2, need_dwarf1;

         if (phdr_dnent == 0
             || !ML_(img_valid)(dimg, phdr_dioff,
                                phdr_dnent * phdr_dent_szB)) {
            ML_(symerr)(di, True,
                        "Missing or invalid ELF Program Header Table"
                        " (debuginfo file)");
            goto out;
         }

         if (shdr_dnent == 0
             || !ML_(img_valid)(dimg, shdr_dioff,
                                shdr_dnent * shdr_dent_szB)) {
            ML_(symerr)(di, True,
                        "Missing or invalid ELF Section Header Table"
                        " (debuginfo file)");
            goto out;
         }

         /* Also find the section header's string table, and validate. */
         /* checked previously by is_elf_object_file: */
         vg_assert(ehdr_dimg.e_shstrndx != SHN_UNDEF);

         // shdr_dioff is the offset of the section header table
         // and we need the ehdr_dimg.e_shstrndx'th entry
         { ElfXX_Shdr a_shdr;
           ML_(img_get)(&a_shdr, dimg,
                        INDEX_BIS(shdr_dioff, ehdr_dimg.e_shstrndx,
                                              shdr_dent_szB),
                        sizeof(a_shdr));
           shdr_strtab_dioff = a_shdr.sh_offset;
           if (!ML_(img_valid)(dimg, shdr_strtab_dioff,
                               1/*bogus, but we don't know the real size*/)) {
              ML_(symerr)(di, True, 
                          "Invalid ELF Section Header String Table"
                          " (debuginfo file)");
              goto out;
           }
         }

         for (i = 0; i < ehdr_dimg.e_phnum; i++) {
            ElfXX_Phdr a_phdr;
            ML_(img_get)(&a_phdr, dimg, INDEX_BIS(ehdr_dimg.e_phoff,
                                                  i, phdr_dent_szB), 
                           sizeof(a_phdr));
            if (a_phdr.p_type == PT_LOAD) {
               for (j = 0; j < VG_(sizeXA)(di->fsm.maps); j++) {
                  struct _DebugInfoMapping* map = VG_(indexXA)(di->fsm.maps, j);
                  if (   a_phdr.p_offset >= map->foff
                      && a_phdr.p_offset <  map->foff + map->size
                      && a_phdr.p_offset + a_phdr.p_filesz
                         < map->foff + map->size) {
                     if (map->rx && rx_dsvma_limit == 0) {
                        rx_dsvma_limit = a_phdr.p_vaddr + a_phdr.p_memsz;
                        rx_dbias = map->avma - map->foff + a_phdr.p_offset
                                   - a_phdr.p_vaddr;
                     }
                     if (map->rw && rw_dsvma_limit == 0) {
                        rw_dsvma_limit = a_phdr.p_vaddr + a_phdr.p_memsz;
                        rw_dbias = map->avma - map->foff + a_phdr.p_offset
                                   - a_phdr.p_vaddr;
                     }
                     break;
                  }
               }
            }
         }

         need_symtab = (symtab_escn.img == NULL);
         need_stabs  = (stab_escn.img == NULL);
         need_dwarf2 = (debug_info_escn.img == NULL);
         need_dwarf1 = (dwarf1d_escn.img == NULL);

         /* Find all interesting sections in the debug image */
         for (i = 0; i < ehdr_dimg.e_shnum; i++) {

            /* Find debug svma and bias information for sections
               we found in the main file. */ 

#           define FIND(_sec, _seg) \
            do { \
               ElfXX_Shdr a_shdr; \
               ML_(img_get)(&a_shdr, dimg, \
                            INDEX_BIS(shdr_dioff, i, shdr_dent_szB), \
                            sizeof(a_shdr)); \
               if (di->_sec##_present \
                   && 0 == ML_(img_strcmp_c)(dimg, shdr_strtab_dioff \
                                             + a_shdr.sh_name, "." #_sec)) { \
                  vg_assert(di->_sec##_size == a_shdr.sh_size); \
                  /* JRS 2013-Jun-01: the following assert doesn't contain */ \
                  /* any ==s, which seems to me to be suspicious. */ \
                  vg_assert(di->_sec##_avma +  a_shdr.sh_addr + _seg##_dbias); \
                  /* Assume we have a correct value for the main */ \
                  /* object's bias.  Use that to derive the debuginfo */ \
                  /* object's bias, by adding the difference in SVMAs */ \
                  /* for the corresponding sections in the two files. */ \
                  /* That should take care of all prelinking effects. */ \
                  di->_sec##_debug_svma = a_shdr.sh_addr; \
                  di->_sec##_debug_bias \
                     = di->_sec##_bias + \
                       di->_sec##_svma - di->_sec##_debug_svma; \
                  TRACE_SYMTAB("acquiring ." #_sec \
                               " debug svma = %#lx .. %#lx\n",       \
                               di->_sec##_debug_svma, \
                               di->_sec##_debug_svma + di->_sec##_size - 1); \
                  TRACE_SYMTAB("acquiring ." #_sec " debug bias = %#lx\n", \
                               di->_sec##_debug_bias); \
               } \
            } while (0);

            /* SECTION   SEGMENT */
            FIND(text,   rx)
            FIND(data,   rw)
            FIND(sdata,  rw)
            FIND(rodata, rw)
            FIND(bss,    rw)
            FIND(sbss,   rw)

#           undef FIND

            /* Same deal as previous FIND, except only do it for those
               sections which we didn't find in the main file. */

#           define FIND(_condition, _sec_name, _sec_escn) \
            do { \
               ElfXX_Shdr a_shdr; \
               ML_(img_get)(&a_shdr, dimg, \
                            INDEX_BIS(shdr_dioff, i, shdr_dent_szB), \
                            sizeof(a_shdr)); \
               if (_condition \
                   && 0 == ML_(img_strcmp_c)(dimg, shdr_strtab_dioff \
                                             + a_shdr.sh_name, _sec_name)) { \
                  Bool nobits; \
                  if (_sec_escn.img != NULL) { \
                     ML_(symerr)(di, True, \
                                 "   debuginfo section duplicates a" \
                                 " section in the main ELF file"); \
                     goto out; \
                  } \
                  _sec_escn.img  = dimg; \
                  _sec_escn.ioff = (DiOffT)a_shdr.sh_offset;  \
                  _sec_escn.szB  = a_shdr.sh_size; \
                  nobits         = a_shdr.sh_type == SHT_NOBITS; \
                  vg_assert(_sec_escn.img  != NULL); \
                  vg_assert(_sec_escn.ioff != DiOffT_INVALID); \
                  TRACE_SYMTAB( "%18s: dioff %llu .. %llu\n", \
                                _sec_name, \
                                (ULong)_sec_escn.ioff, \
                                ((ULong)_sec_escn.ioff) + _sec_escn.szB - 1); \
                  /* SHT_NOBITS sections have zero size in the file. */ \
                  if (a_shdr.sh_offset \
                      + (nobits ? 0 : _sec_escn.szB) > ML_(img_size)(dimg)) { \
                     ML_(symerr)(di, True, \
                                 "   section beyond image end?!"); \
                     goto out; \
                  } \
               } \
            } while (0);

            /* NEEDED?        NAME             ElfSec */
            FIND(need_symtab, ".symtab",       symtab_escn)
            FIND(need_symtab, ".strtab",       strtab_escn)
            FIND(need_stabs,  ".stab",         stab_escn)
            FIND(need_stabs,  ".stabstr",      stabstr_escn)
            FIND(need_dwarf2, ".debug_line",   debug_line_escn)
            FIND(need_dwarf2, ".debug_info",   debug_info_escn)
            FIND(need_dwarf2, ".debug_types",  debug_types_escn)

            FIND(need_dwarf2, ".debug_abbrev", debug_abbv_escn)
            FIND(need_dwarf2, ".debug_str",    debug_str_escn)
            FIND(need_dwarf2, ".debug_ranges", debug_ranges_escn)

            FIND(need_dwarf2, ".debug_loc",    debug_loc_escn)
            FIND(need_dwarf2, ".debug_frame",  debug_frame_escn)

            FIND(need_dwarf2, ".gnu_debugaltlink", debugaltlink_escn)

            FIND(need_dwarf1, ".debug",        dwarf1d_escn)
            FIND(need_dwarf1, ".line",         dwarf1l_escn)

#           undef FIND
         } /* Find all interesting sections */
      } /* do we have a debug image? */

      /* TOPLEVEL */
      /* Look for alternate debug image, and if found, connect |aimg|
         to it. */
      vg_assert(aimg == NULL);

      if (debugaltlink_escn.img != NULL) {
         UInt buildid_offset = ML_(img_strlen)(debugaltlink_escn.img, 0)+1;

         vg_assert(buildid_offset < debugaltlink_escn.szB);

         HChar *altbuildid
            = ML_(dinfo_zalloc)("di.fbi.4",
                                (debugaltlink_escn.szB - buildid_offset)
                                * 2 + 1);

         for (j = 0; j < debugaltlink_escn.szB - buildid_offset; j++)
            VG_(sprintf)(
               altbuildid + 2 * j, "%02x",
               (UInt)ML_(img_get_UChar)(debugaltlink_escn.img,
                                        debugaltlink_escn.ioff 
                                        + buildid_offset + j));

         /* See if we can find a matching debug file */
         aimg = find_debug_file( di, di->fsm.filename, altbuildid,
                                 NULL, 0, True );

         ML_(dinfo_free)(altbuildid);
      }

      /* TOPLEVEL */
      /* If we were successful in finding alternate debug image, pull various
         size and image addresses out of it. */
      if (aimg != NULL && is_elf_object_file_by_DiImage(aimg, True)) {

         /* Pull out and validate program header and section header info */
         DiOffT      ehdr_aioff = 0;
         ElfXX_Ehdr  ehdr_aimg;
         ML_(img_get)(&ehdr_aimg, aimg, ehdr_aioff, sizeof(ehdr_aimg));

         DiOffT   shdr_aioff        = ehdr_aimg.e_shoff;
         UWord    shdr_anent        = ehdr_aimg.e_shnum;
         UWord    shdr_aent_szB     = ehdr_aimg.e_shentsize;

         DiOffT   shdr_strtab_aioff = DiOffT_INVALID;

         if (shdr_anent == 0
             || !ML_(img_valid)(aimg, shdr_aioff,
                                shdr_anent * shdr_aent_szB)) {
            ML_(symerr)(di, True,
                        "Missing or invalid ELF Section Header Table"
                        " (alternate debuginfo file)");
            goto out;
         }

         /* Also find the section header's string table, and validate. */
         /* checked previously by is_elf_object_file: */
         vg_assert(ehdr_aimg.e_shstrndx != SHN_UNDEF);

         // shdr_aioff is the offset of the section header table
         // and we need the ehdr_aimg.e_shstrndx'th entry
         { ElfXX_Shdr a_shdr;
           ML_(img_get)(&a_shdr, aimg,
                        INDEX_BIS(shdr_aioff, ehdr_aimg.e_shstrndx,
                                              shdr_aent_szB),
                        sizeof(a_shdr));
           shdr_strtab_aioff = a_shdr.sh_offset;
           if (!ML_(img_valid)(aimg, shdr_strtab_aioff,
                               1/*bogus, but we don't know the real size*/)) {
              ML_(symerr)(di, True, 
                          "Invalid ELF Section Header String Table"
                          " (alternate debuginfo file)");
              goto out;
           }
         }

         /* Find all interesting sections */
         for (i = 0; i < ehdr_aimg.e_shnum; i++) {

#           define FIND(_sec_name, _sec_escn) \
            do { \
               ElfXX_Shdr a_shdr; \
               ML_(img_get)(&a_shdr, aimg, \
                            INDEX_BIS(shdr_aioff, i, shdr_aent_szB), \
                            sizeof(a_shdr)); \
               if (0 == ML_(img_strcmp_c)(aimg, shdr_strtab_aioff \
                                          + a_shdr.sh_name, _sec_name)) { \
                  if (_sec_escn.img != NULL) { \
                     ML_(symerr)(di, True, \
                                 "   alternate debuginfo section duplicates a" \
                                 " section in the main ELF file"); \
                     goto out; \
                  } \
                  _sec_escn.img  = aimg; \
                  _sec_escn.ioff = (DiOffT)a_shdr.sh_offset; \
                  _sec_escn.szB  = a_shdr.sh_size; \
                  vg_assert(_sec_escn.img  != NULL); \
                  vg_assert(_sec_escn.ioff != DiOffT_INVALID); \
                  TRACE_SYMTAB( "%18s: aioff %llu .. %llu\n", \
                                _sec_name, \
                                (ULong)_sec_escn.ioff, \
                                ((ULong)_sec_escn.ioff) + _sec_escn.szB - 1); \
               } \
            } while (0);

            /*   NAME             ElfSec */
            FIND(".debug_line",   debug_line_alt_escn)
            FIND(".debug_info",   debug_info_alt_escn)
            FIND(".debug_abbrev", debug_abbv_alt_escn)
            FIND(".debug_str",    debug_str_alt_escn)

#           undef FIND
         } /* Find all interesting sections */
      } /* do we have a debug image? */


      /* TOPLEVEL */
      /* Check some sizes */
      vg_assert((dynsym_escn.szB % sizeof(ElfXX_Sym)) == 0);
      vg_assert((symtab_escn.szB % sizeof(ElfXX_Sym)) == 0);

      /* Read symbols */
      {
         void (*read_elf_symtab)(struct _DebugInfo*, const HChar*,
                                 DiSlice*, DiSlice*, DiSlice*, Bool);
         Bool symtab_in_debug;
#        if defined(VGP_ppc64_linux)
         read_elf_symtab = read_elf_symtab__ppc64_linux;
#        else
         read_elf_symtab = read_elf_symtab__normal;
#        endif
         symtab_in_debug = symtab_escn.img == dimg;
         read_elf_symtab(di, "symbol table",
                         &symtab_escn, &strtab_escn, &opd_escn,
                         symtab_in_debug);
         read_elf_symtab(di, "dynamic symbol table",
                         &dynsym_escn, &dynstr_escn, &opd_escn,
                         False);
      } /* Read symbols */

      /* TOPLEVEL */
      /* Read .eh_frame and .debug_frame (call-frame-info) if any.  Do
         the .eh_frame section(s) first. */
      vg_assert(di->n_ehframe >= 0 && di->n_ehframe <= N_EHFRAME_SECTS);
      for (i = 0; i < di->n_ehframe; i++) {
         /* see Comment_on_EH_FRAME_MULTIPLE_INSTANCES above for why
            this next assertion should hold. */
         vg_assert(ML_(sli_is_valid)(ehframe_escn[i]));
         vg_assert(ehframe_escn[i].szB == di->ehframe_size[i]);
         ML_(read_callframe_info_dwarf3)( di,
                                          ehframe_escn[i],
                                          di->ehframe_avma[i],
                                          True/*is_ehframe*/ );
      }
      if (ML_(sli_is_valid)(debug_frame_escn)) {
         ML_(read_callframe_info_dwarf3)( di,
                                          debug_frame_escn,
                                          0/*assume zero avma*/,
                                          False/*!is_ehframe*/ );
      }

      /* Read the stabs and/or dwarf2 debug information, if any.  It
         appears reading stabs stuff on amd64-linux doesn't work, so
         we ignore it.  On s390x stabs also doesnt work and we always
         have the dwarf info in the eh_frame.  We also segfault on
         ppc64-linux when reading stabs, so skip that.  ppc32-linux
         seems OK though.  Also skip on Android. */
#     if !defined(VGP_amd64_linux) \
         && !defined(VGP_s390x_linux) \
         && !defined(VGP_ppc64_linux) \
         && !defined(VGPV_arm_linux_android) \
         && !defined(VGPV_x86_linux_android) \
         && !defined(VGP_mips64_linux)
#if 0
      if (stab_img && stabstr_img) {
         ML_(read_debuginfo_stabs) ( di, stab_img, stab_sz, 
                                         stabstr_img, stabstr_sz );
      }
#endif
#     endif
      /* jrs 2006-01-01: icc-8.1 has been observed to generate
         binaries without debug_str sections.  Don't preclude
         debuginfo reading for that reason, but, in
         read_unitinfo_dwarf2, do check that debugstr is non-NULL
         before using it. */
      if (ML_(sli_is_valid)(debug_info_escn) 
          && ML_(sli_is_valid)(debug_abbv_escn)
          && ML_(sli_is_valid)(debug_line_escn)) {
         /* The old reader: line numbers and unwind info only */
         ML_(read_debuginfo_dwarf3) ( di,
                                      debug_info_escn,
                                      debug_types_escn,
                                      debug_abbv_escn,
                                      debug_line_escn,
                                      debug_str_escn,
                                      debug_str_alt_escn );
         /* The new reader: read the DIEs in .debug_info to acquire
            information on variable types and locations.  But only if
            the tool asks for it, or the user requests it on the
            command line. */
         if (VG_(needs).var_info /* the tool requires it */
             || VG_(clo_read_var_info) /* the user asked for it */) {
            ML_(new_dwarf3_reader)(
               di, debug_info_escn,     debug_types_escn,
                   debug_abbv_escn,     debug_line_escn,
                   debug_str_escn,      debug_ranges_escn,
                   debug_loc_escn,      debug_info_alt_escn,
                   debug_abbv_alt_escn, debug_line_alt_escn,
                   debug_str_alt_escn
            );
         }
      }
#if 0
      if (dwarf1d_img && dwarf1l_img) {
         ML_(read_debuginfo_dwarf1) ( di, dwarf1d_img, dwarf1d_sz, 
                                          dwarf1l_img, dwarf1l_sz );
      }
#endif
      /* TOPLEVEL */

   } /* "Find interesting sections, read the symbol table(s), read any debug
        information" (a local scope) */

   /* TOPLEVEL */
   res = True;

   /* If reading Dwarf3 variable type/location info, print a line
      showing the number of variables read for each object.
      (Currently disabled -- is a sanity-check mechanism for
      exp-sgcheck.) */
   if (0 && (VG_(needs).var_info || VG_(clo_read_var_info))) {
      UWord nVars = 0;
      if (di->varinfo) {
         for (j = 0; j < VG_(sizeXA)(di->varinfo); j++) {
            OSet* /* of DiAddrRange */ scope
               = *(OSet**)VG_(indexXA)(di->varinfo, j);
            vg_assert(scope);
            VG_(OSetGen_ResetIter)( scope );
            while (True) {
               DiAddrRange* range  = VG_(OSetGen_Next)( scope );
               if (!range) break;
               vg_assert(range->vars);
               Word w = VG_(sizeXA)(range->vars);
               vg_assert(w >= 0);
               if (0) VG_(printf)("range %#lx %#lx %ld\n",
                                  range->aMin, range->aMax, w);
               nVars += (UWord)w;
            }
         }
      }
      VG_(umsg)("VARINFO: %7lu vars   %7ld text_size   %s\n",
                nVars, di->text_size, di->fsm.filename);
   }
   /* TOPLEVEL */

  out: 
   {
      /* Last, but not least, detach from the image(s). */
      if (mimg) ML_(img_done)(mimg);
      if (dimg) ML_(img_done)(dimg);
      if (aimg) ML_(img_done)(aimg);

      if (svma_ranges) VG_(deleteXA)(svma_ranges);

      return res;
   } /* out: */ 

   /* NOTREACHED */
}

#endif // defined(VGO_linux)

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/