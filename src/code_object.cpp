/* The University of Illinois/NCSA
   Open Source License (NCSA)

   Copyright (c) 2020, Advanced Micro Devices, Inc. All rights reserved.

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to
   deal with the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

    - Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimers.
    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimers in
      the documentation and/or other materials provided with the distribution.
    - Neither the names of Advanced Micro Devices, Inc,
      nor the names of its contributors may be used to endorse or promote
      products derived from this Software without specific prior written
      permission.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
   THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS WITH THE SOFTWARE.  */

#include "code_object.h"
#include "debug.h"
#include "logging.h"

#include <ctype.h>
#include <cxxabi.h>
#include <elf.h>
#include <elfutils/libdw.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace amd::debug_agent
{

code_object_t::code_object_t (amd_dbgapi_process_id_t process_id,
                              amd_dbgapi_code_object_id_t code_object_id)
    : m_code_object_id (code_object_id), m_process_id (process_id)
{
  if (amd_dbgapi_code_object_get_info (
          process_id, code_object_id, AMD_DBGAPI_CODE_OBJECT_INFO_LOAD_ADDRESS,
          sizeof (m_load_address), &m_load_address)
      != AMD_DBGAPI_STATUS_SUCCESS)
    {
      agent_warning ("could not get the code object's load address");
      return;
    }

  char *value;
  if (amd_dbgapi_code_object_get_info (m_process_id, m_code_object_id,
                                       AMD_DBGAPI_CODE_OBJECT_INFO_URI_NAME,
                                       sizeof (value), &value)
      != AMD_DBGAPI_STATUS_SUCCESS)
    {
      agent_warning ("could not get the code object's URI");
      return;
    }

  m_uri.assign (value);
  free (value);
}

code_object_t::code_object_t (code_object_t &&rhs)
    : m_load_address (rhs.m_load_address), m_mem_size (rhs.m_mem_size),
      m_uri (std::move (rhs.m_uri)), m_code_object_id (rhs.m_code_object_id),
      m_process_id (rhs.m_process_id)
{
  m_fd = rhs.m_fd;
  rhs.m_fd.reset ();
}

code_object_t::~code_object_t ()
{
  if (m_fd)
    ::close (*m_fd);
}

std::optional<code_object_t::symbol_info_t>
code_object_t::find_symbol (amd_dbgapi_global_address_t address)
{
  /* Load the symbol table.  */
  load_symbol_map ();

  if (auto it = m_symbol_map->upper_bound (address);
      it != m_symbol_map->begin ())
    {
      if (auto &&[symbol_value, symbol] = *std::prev (it);
          address < (symbol_value + symbol.second))
        {
          std::string symbol_name = symbol.first;

          if (int status; auto *demangled_name = abi::__cxa_demangle (
                              symbol_name.c_str (), nullptr, nullptr, &status))
            {
              symbol_name = demangled_name;
              free (demangled_name);
            }

          return symbol_info_t{ std::move (symbol_name), symbol_value,
                                symbol.second };
        }
    }

  return {};
}

void
code_object_t::open ()
{
  const std::string protocol_delim{ "://" };

  size_t protocol_end = m_uri.find (protocol_delim);
  std::string protocol = m_uri.substr (0, protocol_end);
  protocol_end += protocol_delim.length ();

  std::transform (protocol.begin (), protocol.end (), protocol.begin (),
                  [] (unsigned char c) { return std::tolower (c); });

  std::string path;
  size_t path_end = m_uri.find_first_of ("#?", protocol_end);
  if (path_end != std::string::npos)
    path = m_uri.substr (protocol_end, path_end++ - protocol_end);
  else
    path = m_uri.substr (protocol_end);

  /* %-decode the string.  */
  std::string decoded_path;
  decoded_path.reserve (path.length ());
  for (size_t i = 0; i < path.length (); ++i)
    if (path[i] == '%' && std::isxdigit (path[i + 1])
        && std::isxdigit (path[i + 2]))
      {
        decoded_path += std::stoi (path.substr (i + 1, 2), 0, 16);
        i += 2;
      }
    else
      decoded_path += path[i];

  /* Tokenize the query/fragment.  */
  std::vector<std::string> tokens;
  size_t pos, last = path_end;
  while ((pos = m_uri.find ('&', last)) != std::string::npos)
    {
      tokens.emplace_back (m_uri.substr (last, pos - last));
      last = pos + 1;
    }
  if (last != std::string::npos)
    tokens.emplace_back (m_uri.substr (last));

  /* Create a tag-value map from the tokenized query/fragment.  */
  std::unordered_map<std::string, std::string> params;
  std::for_each (tokens.begin (), tokens.end (), [&] (std::string &token) {
    size_t delim = token.find ('=');
    if (delim != std::string::npos)
      params.emplace (token.substr (0, delim), token.substr (delim + 1));
  });

  std::vector<char> buffer;
  try
    {
      size_t offset{ 0 }, size{ 0 };

      if (auto offset_it = params.find ("offset"); offset_it != params.end ())
        offset = std::stoul (offset_it->second, nullptr, 0);

      if (auto size_it = params.find ("size"); size_it != params.end ())
        if (!(size = std::stoul (size_it->second, nullptr, 0)))
          return;

      if (protocol == "file")
        {
          std::ifstream file (decoded_path, std::ios::in | std::ios::binary);
          if (!file)
            {
              agent_warning ("could not open `%s'", decoded_path.c_str ());
              return;
            }

          if (!size)
            {
              file.ignore (std::numeric_limits<std::streamsize>::max ());
              size_t bytes = file.gcount ();
              file.clear ();

              if (bytes < offset)
                {
                  agent_warning ("invalid uri `%s' (file size < offset)",
                                 decoded_path.c_str ());
                  return;
                }
              size = bytes - offset;
            }

          file.seekg (offset, std::ios_base::beg);
          buffer.resize (size);
          file.read (&buffer[0], size);
        }
      else if (protocol == "memory")
        {
          if (!offset || !size)
            {
              agent_warning ("invalid uri `%s' (offset and size must be != 0",
                             m_uri.c_str ());
              return;
            }

          buffer.resize (size);
          if (amd_dbgapi_read_memory (m_process_id, AMD_DBGAPI_WAVE_NONE, 0,
                                      AMD_DBGAPI_ADDRESS_SPACE_GLOBAL, offset,
                                      &size, buffer.data ())
              != AMD_DBGAPI_STATUS_SUCCESS)
            {
              agent_warning ("could not read memory at 0x%lx", offset);
              return;
            }
        }
      else
        {
          agent_warning ("\"%s\" protocol not supported", protocol.c_str ());
          return;
        }
    }
  catch (...)
    {
    }

  int fd = ::open ("/tmp", O_TMPFILE | O_RDWR, 0666);
  if (fd == -1)
    {
      agent_warning ("could not create a temporary file for code object");
      return;
    }

  if (size_t size = ::write (fd, buffer.data (), buffer.size ());
      size != buffer.size ())
    {
      agent_warning ("could not write to the temporary file");
      return;
    }

  ::lseek (fd, 0, SEEK_SET);

  /* Calculate the size of the code object as loaded in memory.  Its size is
     the distance of the end of the highest segment from the load address.  */
  std::unique_ptr<Elf, void (*) (Elf *)> elf (
      elf_begin (fd, ELF_C_READ, nullptr), [] (Elf *elf) { elf_end (elf); });
  if (!elf)
    {
      agent_warning ("elf_begin failed for `%s'", m_uri.c_str ());
      return;
    }

  size_t phnum;
  if (elf_getphdrnum (elf.get (), &phnum) != 0)
    {
      agent_warning ("elf_getphdrnum failed for `%s'", m_uri.c_str ());
      return;
    }

  for (size_t i = 0; i < phnum; ++i)
    {
      GElf_Phdr phdr_mem;
      GElf_Phdr *phdr = gelf_getphdr (elf.get (), i, &phdr_mem);
      if (!phdr)
        {
          agent_warning ("gelf_getphdr failed for `%s'", m_uri.c_str ());
          return;
        }

      if (phdr->p_type == PT_LOAD)
        m_mem_size = std::max (m_mem_size, phdr->p_vaddr + phdr->p_memsz);
    }

  m_fd.emplace (fd);
}

namespace
{

const std::vector<std::string> *
get_source_file_index (const std::string &file_name)
{
  static std::unordered_map<std::string, std::vector<std::string>> file_map;

  if (auto it = file_map.find (file_name); it != file_map.end ())
    return &it->second;

  std::ifstream file (file_name);
  if (!file)
    return {};

  auto [it, success]
      = file_map.emplace (file_name, std::vector<std::string>{});
  agent_assert (success && "emplace should have succeeded");

  auto &lines = it->second;
  std::string line;

  while (std::getline (file, line))
    lines.emplace_back (line);

  return &lines;
}

} /* namespace */

void
code_object_t::load_symbol_map ()
{
  agent_assert (is_open () && "code object is not opened");

  if (m_symbol_map)
    return;

  std::unique_ptr<Elf, void (*) (Elf *)> elf (
      elf_begin (*m_fd, ELF_C_READ, nullptr),
      [] (Elf *elf) { elf_end (elf); });

  if (!elf)
    return;

  m_symbol_map.emplace ();

  /* Slurp the symbol table.  */
  Elf_Scn *scn = nullptr;
  while ((scn = elf_nextscn (elf.get (), scn)) != nullptr)
    {
      GElf_Shdr shdr_mem;
      GElf_Shdr *shdr = gelf_getshdr (scn, &shdr_mem);
      if (shdr->sh_type != SHT_SYMTAB && shdr->sh_type != SHT_DYNSYM)
        continue;

      Elf_Data *data = elf_getdata (scn, nullptr);
      if (!data)
        continue;

      size_t symbol_count
          = data->d_size / gelf_fsize (elf.get (), ELF_T_SYM, 1, EV_CURRENT);
      for (size_t j = 0; j < symbol_count; ++j)
        {
          GElf_Sym sym_mem;
          GElf_Sym *sym = gelf_getsym (data, j, &sym_mem);

          if (GELF_ST_TYPE (sym->st_info) != STT_FUNC
              || sym->st_shndx == SHN_UNDEF)
            continue;

          std::string symbol_name{ elf_strptr (elf.get (), shdr->sh_link,
                                               sym->st_name) };

          auto [it, success] = m_symbol_map->emplace (
              m_load_address + sym->st_value,
              std::make_pair (symbol_name, sym->st_size));

          /* If there already was a symbol defined at this address, but this
             new symbol covers a larger address range, replace the old symbol
             with this new one.  */
          if (!success && sym->st_size > it->second.second)
            it->second = std::make_pair (symbol_name, sym->st_size);
        }
    }

  /* TODO: If we did not see a symbtab, check the dynamic segment.  */
}

void
code_object_t::load_debug_info ()
{
  agent_assert (is_open () && "code object is not opened");

  if (m_line_number_map && m_compilation_unit_low_high_pc_map)
    return;

  std::unique_ptr<Dwarf, void (*) (Dwarf *)> dbg (
      dwarf_begin (*m_fd, DWARF_C_READ), [] (Dwarf *dbg) { dwarf_end (dbg); });

  if (!dbg)
    return;

  m_line_number_map.emplace ();
  m_compilation_unit_low_high_pc_map.emplace ();

  Dwarf_Off cu_offset{ 0 }, next_offset;
  size_t header_size;

  while (!dwarf_nextcu (dbg.get (), cu_offset, &next_offset, &header_size,
                        nullptr, nullptr, nullptr))
    {
      Dwarf_Die die;
      if (!dwarf_offdie (dbg.get (), cu_offset + header_size, &die))
        continue;

      Dwarf_Addr lowpc = 0;
      Dwarf_Addr highpc = 0;

      if (!dwarf_lowpc (&die, &lowpc) && !dwarf_highpc (&die, &highpc))
        m_compilation_unit_low_high_pc_map->emplace (m_load_address + lowpc,
                                                     m_load_address + highpc);

      Dwarf_Lines *lines;
      size_t line_count;
      if (dwarf_getsrclines (&die, &lines, &line_count))
        continue;

      for (size_t index = 0; index < line_count; ++index)
        {
          Dwarf_Line *line = dwarf_onesrcline (lines, index);

          Dwarf_Addr addr;
          if (dwarf_lineaddr (line, &addr))
            addr = 0;

          auto file = dwarf_linesrc (line, nullptr, nullptr);

          int line_number;
          if (dwarf_lineno (line, &line_number))
            line_number = 0;

          if (addr && line_number)
            m_line_number_map->emplace (
                m_load_address + addr,
                std::make_pair (std::string (file), line_number));
        }

      cu_offset = next_offset;
    }
}

void
code_object_t::disassemble (amd_dbgapi_architecture_id_t architecture_id,
                            amd_dbgapi_global_address_t pc)
{
  amd_dbgapi_size_t largest_instruction_size;
  if (amd_dbgapi_architecture_get_info (
          architecture_id,
          AMD_DBGAPI_ARCHITECTURE_INFO_LARGEST_INSTRUCTION_SIZE,
          sizeof (largest_instruction_size), &largest_instruction_size)
      != AMD_DBGAPI_STATUS_SUCCESS)
    agent_error ("could not get the instruction size from the architecture");

  /* Load the line number table, and low/high pc for all CUs.  */
  load_debug_info ();

  constexpr int context_byte_size = 32;
  amd_dbgapi_global_address_t start_pc;

  /* Try to find a line number that precedes `pc` by `context_byte_size` bytes.
     If we don't have a line number map, simply subtract an offset from the pc,
     hoping that we'll land on a valid instruction, or that the disassembler
     will right itself after disassembling a few instructions.  */

  if (auto it = m_line_number_map->upper_bound (pc);
      it != m_line_number_map->begin ())
    {
      do
        {
          it = std::prev (it);
          if ((pc - it->first) >= context_byte_size)
            break;
        }
      while (it != m_line_number_map->begin ());

      start_pc = it->first;
    }
  else
    {
      /* Don't print any instructions before the current pc.  The instructions
         are of variable size so we can't reliably tell if we'll land on a
         valid instruction.  */
      start_pc = pc;
    }

  amd_dbgapi_global_address_t end_pc = pc + context_byte_size;

  /* If pc is included in a [lowpc,highpc] interval, clamp start_pc and
     end_pc.  */

  if (auto it = m_compilation_unit_low_high_pc_map->upper_bound (pc);
      it != m_compilation_unit_low_high_pc_map->begin ())
    {
      if (auto [low_pc, high_pc] = *std::prev (it); pc < high_pc)
        {
          start_pc = std::max (start_pc, low_pc);
          end_pc = std::min (end_pc, high_pc);
        }
    }

  auto symbol = find_symbol (pc);

  agent_out << std::endl << "Disassembly";
  if (symbol)
    agent_out << " for function " << symbol->m_name;
  agent_out << ":" << std::endl;

  agent_out << "    code object: " << m_uri << std::endl;
  agent_out << "    loaded at: "
            << "[0x" << std::hex << m_load_address << "-"
            << "0x" << std::hex << (m_load_address + m_mem_size) << "]"
            << std::endl;

  std::string prev_file_name;
  size_t prev_line_number{ 0 };

  for (amd_dbgapi_global_address_t addr = start_pc; addr < end_pc;)
    {
      if (auto it = m_line_number_map->find (addr);
          it != m_line_number_map->end ())
        {
          const std::string &file_name = it->second.first;
          size_t line_number = it->second.second;

          if (file_name != prev_file_name || line_number != prev_line_number)
            agent_out << std::endl;

          if (file_name != prev_file_name)
            agent_out << file_name << ":" << std::endl;

          if (line_number != prev_line_number)
            {
              size_t first_line = line_number;
              size_t last_line = line_number;

              /* Find the first line to print between prev_line_number and
                 line_number that does not appear in the line number table.
               */
              if (file_name == prev_file_name
                  && line_number > prev_line_number)
                {
                  while (--first_line > prev_line_number)
                    {
                      if (std::find_if (
                              m_line_number_map->begin (),
                              m_line_number_map->end (),
                              [first_line, &file_name] (
                                  const std::remove_reference_t<decltype (
                                      *m_line_number_map)>::value_type
                                      &value) {
                                return file_name == value.second.first
                                       && first_line == value.second.second;
                              })
                          != m_line_number_map->end ())
                        break;
                    }
                  /* First is either prev_line_number, or a line associated
                     with another address, so start at the next line.  */
                  ++first_line;
                }

              for (size_t line = first_line; line <= last_line; ++line)
                {
                  agent_out << std::setfill (' ') << std::setw (8) << std::left
                            << std::dec << line;

                  if (auto *lines = get_source_file_index (file_name); !lines)
                    agent_out << file_name << ": No such file or directory.";
                  else if (line <= lines->size ())
                    agent_out << lines->at (line - 1);

                  agent_out << std::endl;
                }
            }

          prev_file_name = file_name;
          prev_line_number = line_number;
        }

      std::vector<uint8_t> buffer (largest_instruction_size);

      amd_dbgapi_size_t size = buffer.size ();
      if (amd_dbgapi_read_memory (
              m_process_id, AMD_DBGAPI_WAVE_NONE, AMD_DBGAPI_LANE_NONE,
              AMD_DBGAPI_ADDRESS_SPACE_GLOBAL, addr, &size, buffer.data ())
          != AMD_DBGAPI_STATUS_SUCCESS)
        {
          agent_out << "Cannot access memory at address 0x" << std::hex << addr
                    << std::endl;
          break;
        }

      auto symbolizer = [] (amd_dbgapi_symbolizer_id_t symbolizer_id,
                            amd_dbgapi_global_address_t address,
                            char **symbol_text) {
        auto &code_object = *reinterpret_cast<code_object_t *> (symbolizer_id);
        std::stringstream ss;

        ss << "0x" << std::hex << address;

        if (auto &&symbol = code_object.find_symbol (address))
          {
            ss << " <" << symbol->m_name;
            ss << "+" << std::dec << (address - symbol->m_value);
            ss << ">";
          }

        *symbol_text = strdup (ss.str ().c_str ());
        return AMD_DBGAPI_STATUS_SUCCESS;
      };

      char *value;
      if (amd_dbgapi_disassemble_instruction (
              architecture_id, addr, &size, buffer.data (), &value,
              reinterpret_cast<amd_dbgapi_symbolizer_id_t> (this), symbolizer)
          != AMD_DBGAPI_STATUS_SUCCESS)
        agent_error ("amd_dbgapi_disassemble_instruction failed");

      std::string instruction (value);
      free (value);

      agent_out << ((addr == pc) ? " => " : "    ");

      agent_out << "0x" << std::hex << addr;
      if (symbol)
        {
          agent_out << " <";
          if (addr >= symbol->m_value)
            agent_out << "+" << std::dec << (addr - symbol->m_value);
          else
            agent_out << "-" << std::dec << (symbol->m_value - addr);
          agent_out << ">";
        }

      agent_out << ":    " << instruction << std::endl;

      addr += size;
    }

  agent_out << std::endl << "End of disassembly." << std::endl;
}

bool
code_object_t::save (const std::string &directory) const
{
  agent_assert (is_open () && "code object is not opened");

  std::string name{ m_uri };

  size_t pos{};
  while ((pos = name.find_first_of (":/#?&="), pos) != std::string::npos)
    name[pos] = '_';

  std::string file_path = directory + '/' + name;
  std::ofstream file (file_path, std::ios::out | std::ios::binary);
  std::vector<char> buffer (lseek (*m_fd, 0, SEEK_END));

  ::lseek (*m_fd, 0, SEEK_SET);
  if (size_t size = ::read (*m_fd, buffer.data (), buffer.size ());
      size != buffer.size ())
    return false;

  file.write (buffer.data (), buffer.size ());
  file.close ();

  return file.good ();
}

} /* namespace amd::debug_agent */