// standard library imports
#include <iostream>
#include <string>
#include <filesystem>
// cmdata import
#include "src/resdata/resdata.hpp"
// external library import
#include <popt.h>

int main(int argc, const char** argv)
{
  std::cout << "################################################" << std::endl;
  std::cout << "#################### RESDATA ####################" << std::endl;
  std::cout << "################################################" << std::endl;
  std::cout << "################## Version 0 ################" << std::endl;
  std::cout << "################################################\n" << std::endl;

  double cutoff = 0.55, mol_cutoff = 6.0;
  int nskip = 0, num_threads = 1, mol_threads = -1, dt = 0;
  float t_begin = 0.0, t_end = -1.0;
  char *p_traj_path = NULL, *p_top_path = NULL, *p_mode = NULL, *p_weights_path = NULL, *p_index_path = NULL;
  char *p_out_prefix = NULL;
  std::string traj_path, top_path, mode, weights_path, index_path;
  std::string out_prefix;
  int *p_nopbc = NULL;
  int *p_blk_avg = NULL;
  bool nopbc = false;
  bool blk_avg = false;

  // make popt options
  struct poptOption optionsTable[] = {
    POPT_AUTOHELP
    {"traj",        'f',  POPT_ARG_STRING,                          &p_traj_path,     0, "Trajectory file",             "FILE"},
    {"top",         's',  POPT_ARG_STRING,                          &p_top_path,      0, "Topology file",               "FILE"},
    {"t_begin",     'b',  POPT_ARG_FLOAT | POPT_ARGFLAG_OPTIONAL,   &t_begin,         0, "Start time",                  "FLOAT"},
    {"t_end",       'e',  POPT_ARG_FLOAT | POPT_ARGFLAG_OPTIONAL,   &t_end,           0, "End time",                    "FLOAT"},
    {"out",         'o',  POPT_ARG_STRING | POPT_ARGFLAG_OPTIONAL,  &p_out_prefix,    0, "Output prefix",               "STRING"},
    {"index",       'n',  POPT_ARG_STRING | POPT_ARGFLAG_OPTIONAL,  &p_index_path,    0, "Index file   ",               "FILE"},
    {"dt",          '\0', POPT_ARG_INT | POPT_ARGFLAG_OPTIONAL,     &dt,              0, "Time step",                   "INT"},
    {"cutoff",      '\0', POPT_ARG_DOUBLE | POPT_ARGFLAG_OPTIONAL,  &cutoff,          0, "Cutoff distance",             "DOUBLE"},
    {"mol_cutoff",  '\0', POPT_ARG_DOUBLE | POPT_ARGFLAG_OPTIONAL,  &mol_cutoff,      0, "Molecule cutoff distance",    "DOUBLE"},
    {"nskip",       '\0', POPT_ARG_INT | POPT_ARGFLAG_OPTIONAL,     &nskip,           0, "Number of frames to skip",    "INT"},
    {"num_threads", '\0', POPT_ARG_INT | POPT_ARGFLAG_OPTIONAL,     &num_threads,     0, "Number of threads",           "INT"},
    {"mol_threads", '\0', POPT_ARG_INT | POPT_ARGFLAG_OPTIONAL,     &mol_threads,     0, "Number of molecule threads",  "INT"},
    {"mode",        '\0', POPT_ARG_STRING | POPT_ARGFLAG_OPTIONAL,  &p_mode,          0, "Mode of operation",           "STRING"},
    {"weights",     '\0', POPT_ARG_STRING | POPT_ARGFLAG_OPTIONAL,  &p_weights_path,  0, "Weights file",                "FILE"},
    {"no_pbc",      '\0', POPT_ARG_NONE | POPT_ARGFLAG_OPTIONAL,    &p_nopbc,         0, "Ignore pbcs",                 0},
    {"blk_avg",      '\0', POPT_ARG_NONE | POPT_ARGFLAG_OPTIONAL,   &p_blk_avg,       0, "Block Average",               0},
    POPT_TABLEEND
  };

  // parse options
  poptContext opt_context = poptGetContext("resdata", argc, argv, optionsTable, 0);
  int opt=poptGetNextOpt(opt_context); // needs to be run to parse
  if (opt < -1) {
      /* Handle error condition */
      fprintf(stderr, "%s: %s\n", poptBadOption(opt_context, POPT_BADOPTION_NOALIAS), poptStrerror(opt));
      return 1;
  }
  poptFreeContext(opt_context);

  // check if traj and top are set
  if ( !(p_traj_path && p_top_path) )
  {
    std::cerr << "Trajectory and topology files must be set!" << std::endl;
    return 1;
  }

  traj_path = std::string(p_traj_path);
  top_path = std::string(p_top_path);
  if ( p_index_path != NULL ) index_path = std::string(p_index_path);
  mode = p_mode ? std::string(p_mode) : std::string("intra+same+cross");
  if ( p_weights_path != NULL ) weights_path = std::string(p_weights_path);
  if ( p_out_prefix != NULL ) out_prefix = std::string(p_out_prefix);
  if ( p_nopbc != NULL ) nopbc = true;
  if ( p_blk_avg != NULL ) blk_avg = true;
  // check if paths are valid
  if ( !std::filesystem::exists(std::filesystem::path(traj_path)) )
  {
    std::cerr << "Trajectory file does not exist!" << std::endl;
    return 1;
  }
  if ( !std::filesystem::exists(std::filesystem::path(top_path)) )
  {
    std::cerr << "Topology file does not exist!" << std::endl;
    return 2;
  }
  if ( !weights_path.empty() && !std::filesystem::exists(std::filesystem::path(weights_path)) )
  {
    std::cerr << "Weights file does not exist!" << std::endl;
    return 3;
  }
  if (  !index_path.empty() &&!std::filesystem::exists(std::filesystem::path(index_path)) )
  {
    std::cerr << "Index file does not exist!" << std::endl;
    return 4;
  }
  // if ( !out_prefix.empty() )
  // {
  //   bool created = std::filesystem::create_directories(std::filesystem::path(out_prefix));
  //   if ( !created ) // if not created
  //   {
  //     std::cout << "Could not create output directory at " << out_prefix << std::endl;
  //     if ( std::filesystem::exists(std::filesystem::path(out_prefix)) ) // already exists
  //     {
  //       std::cout << "Reason: directory already exists! WARNING: Files might be overwritten!" << std::endl;
  //     }
  //     else if ( !std::filesystem::is_directory(std::filesystem::path(out_prefix)) ) // not a directory (file or non-existent path)
  //     {
  //       std::cout << "Reason: path is not a directory!" << std::endl;
  //     }
  //     else if ( !std::filesystem::exists(std::filesystem::path(out_prefix)) ) // does not exist (no permissions or non-existent parent directory)
  //     {
  //       std::cout << "Reason: could not create directory!" << std::endl;
  //       return 5;
  //     }
  //   }
  // }
  if ( num_threads < 1 )
  {
    std::cerr << "Number of threads must be at least 1!" << std::endl;
    return 6;
  }
  if ( mol_threads < 1 )
  {
    std::cout << "Setting molecule threads to number of threads!" << std::endl;
    mol_threads = num_threads;
  }
  if ( dt < 0 )
  {
    std::cerr << "Time step must be a positive number!" << std::endl;
    return 7;
  }
  if ( nskip < 0 )
  {
    std::cerr << "Number of frames to skip must be at least 0!" << std::endl;
    return 8;
  }
  if ( cutoff <= 0.0 )
  {
    std::cerr << "Cutoff distance must be greater than 0!" << std::endl;
    return 9;
  }
  if ( mol_cutoff <= 0.0 )
  {
    std::cerr << "Molecule cutoff distance must be greater than 0!" << std::endl;
    return 10;
  }
  if ( t_begin < 0.0 )
  {
    std::cerr << "Start time must be at least 0!" << std::endl;
    return 11;
  }
  if ( t_end < t_begin && t_end != -1.0 )
  {
    std::cerr << "End time must be greater than start time!" << std::endl;
    return 12;
  }

  resdata::RESData resdata(
    top_path, traj_path, cutoff, mol_cutoff, nskip, num_threads, mol_threads, dt,
    mode, weights_path, nopbc, t_begin, t_end, index_path, blk_avg
  );
  resdata.run();
  // cmdata.process_data();
  resdata.write_output(out_prefix);

  return 0;
}
