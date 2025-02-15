#ifndef _CMDATA_CMDATA_HPP
#define _CMDATA_CMDATA_HPP

// gromacs includes
#include <gromacs/trajectoryanalysis/topologyinformation.h>
#include <gromacs/math/vec.h>
#include <gromacs/pbcutil/pbc.h>
#include <gromacs/fileio/tpxio.h>

// cmdata includes
#include "io.hpp"
#include "indexing.hpp"
#include "parallel.hpp"
#include "density.hpp"
#include "mindist.hpp"
#include "xtc_frame.hpp"
#include "function_types.hpp"

// standard library imports
#include <iostream>
#include <omp.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>
#include <memory>
#include <string>
#include <algorithm>
#include <functional>
#include <numeric>
#include <sstream>
#include <fstream>

// xdrfile includes
#include <xdrfile.h>
#include <xdrfile_xtc.h>

namespace cmdata
{

class CMData
{
private:
  // general fields
  float cutoff_, mol_cutoff_, mcut2_, cut_sig_2_;
  int nskip_, n_x_;
  float dt_, t_begin_, t_end_;
  gmx_mtop_t *mtop_;
  rvec *xcm_ = nullptr;

  // molecule number fields
  int nindex_;
  gmx::RangePartitioning mols_;
  std::vector<int> natmol2_;
  std::vector<int> mol_id_;
  std::vector<int> num_mol_unique_;
  std::vector<float> inv_num_mol_unique_;
  std::vector<float> inv_num_mol_;
  std::string bkbn_H_;

  // weights fields
  float weights_sum_;
  std::string weights_path_;
  std::vector<float> weights_;

  // pbc fields
  bool no_pbc_;
  t_pbc *pbc_;
  PbcType pbcType_;

  // frame fields
  cmdata::xtc::Frame *frame_;
  XDRFILE *trj_;

  // density fields
  float dx_;
  std::size_t n_bins_;
  std::vector<float> density_bins_;
  std::vector<std::vector<int>> cross_index_;

  using cmdata_matrix = std::vector<std::vector<std::vector<std::vector<float>>>>;
  cmdata_matrix interm_same_mat_density_;
  cmdata_matrix interm_cross_mat_density_;
  cmdata_matrix intram_mat_density_;
  cmdata_matrix interm_same_maxcdf_mol_;
  cmdata_matrix interm_cross_maxcdf_mol_;

  // temporary containers for maxcdf operations
  std::vector<std::vector<float>> frame_same_mat_;
  std::vector<std::vector<float>> frame_cross_mat_;
  std::vector<std::vector<std::mutex>> frame_same_mutex_;
  std::vector<std::vector<std::mutex>> frame_cross_mutex_;

  // parallelization fields
  int num_threads_;
  int num_mol_threads_;
  std::vector<std::thread> threads_;
  std::vector<std::thread> mol_threads_;
  cmdata::parallel::Semaphore semaphore_;
  std::vector<cmdata::indexing::SameThreadIndices> same_thread_indices_;
  std::vector<cmdata::indexing::CrossThreadIndices> cross_thread_indices_;

  // mode selection, booleans and functions
  std::string mode_;
  bool intra_ = false, same_ = false, cross_ = false;
  bool h5_ = false;

  // function types
  using ftype_intra_ = cmdata::ftypes::function_traits<decltype(&cmdata::density::intra_mol_routine)>;
  using ftype_same_ = cmdata::ftypes::function_traits<decltype(&cmdata::density::inter_mol_same_routine)>;
  using ftype_cross_ = cmdata::ftypes::function_traits<decltype(&cmdata::density::inter_mol_cross_routine)>;

  std::function<ftype_intra_::signature> f_intra_mol_;
  std::function<ftype_same_::signature> f_inter_mol_same_;
  std::function<ftype_cross_::signature> f_inter_mol_cross_;

  static void molecule_routine(
    const int i, const int nindex_, t_pbc *pbc, rvec *x, const std::vector<float> &inv_num_mol_, const float cut_sig_2_, 
    const std::vector<int> &natmol2_, const std::vector<int> &num_mol_unique_, const std::vector<int> &mol_id_, 
    const std::vector<std::vector<int>> &cross_index_, const std::vector<float> &density_bins_, const float mcut2_, 
    rvec *xcm_, const gmx::RangePartitioning &mols_, gmx_mtop_t *mtop_, 
    std::vector<std::vector<float>> &frame_same_mat_, std::vector<std::vector<std::mutex>> &frame_same_mutex_,
    cmdata_matrix &intram_mat_density_, cmdata_matrix &interm_same_mat_density_, std::vector<std::vector<float>> &frame_cross_mat_,
    std::vector<std::vector<std::mutex>> &frame_cross_mutex_, cmdata_matrix &interm_cross_mat_density_, cmdata::parallel::Semaphore &semaphore_,
    const std::function<ftype_intra_::signature> &f_intra_mol_, const std::function<ftype_same_::signature> &f_inter_mol_same_,
    const std::function<ftype_cross_::signature> &f_inter_mol_cross_, float weight, std::string bkbn_H_
  )
  {
    semaphore_.acquire();
    const char * atomname;
    int tmp_i = 0;
    std::size_t mol_i = i, mol_j = 0;
    while ( static_cast<int>(mol_i) - num_mol_unique_[tmp_i] >= 0  )
    {
      mol_i -= num_mol_unique_[tmp_i];
      tmp_i++;
      if (tmp_i == num_mol_unique_.size()) break;
    }
    if (mol_i == num_mol_unique_[mol_id_[i]]) mol_i = 0;
    int molb = 0;
    /* Loop over molecules  */
    for (int j = 0; j < nindex_; j++)
    {
      if (j!=0)
        if (mol_j == num_mol_unique_[mol_id_[j-1]]) mol_j = 0;

      /* intermolecular interactions are evaluated only among neighbour molecules */
      if (i!=j)
      {
        rvec dx;
        if (pbc != nullptr) pbc_dx(pbc, xcm_[i], xcm_[j], dx);
        else rvec_sub(xcm_[i], xcm_[j], dx);
        float dx2 = iprod(dx, dx);
        if (dx2 > mcut2_) continue;
      }
      /* for molecules of different specie we fill half a matrix */
      if (mol_id_[i] != mol_id_[j] && j < i) continue;
      std::size_t a_i = 0;
      // GMX_RELEASE_ASSERT(mols_.numBlocks() > 0, "Cannot access index[] from empty mols");

      /* cycle over the atoms of a molecule i */
      for (std::size_t ii = mols_.block(i).begin(); ii < mols_.block(i).end(); ii++)
      {
        std::size_t a_j = 0;
        mtopGetAtomAndResidueName(*mtop_, ii, &molb, &atomname, nullptr, nullptr, nullptr);
        if (atomname[0] == 'H' && atomname != bkbn_H_)
        {
          a_i++;
          continue;
        }
        /* cycle over the atoms of a molecule j */
        for (std::size_t jj = mols_.block(j).begin(); jj < mols_.block(j).end(); jj++)
        {
          mtopGetAtomAndResidueName(*mtop_, jj, &molb, &atomname, nullptr, nullptr, nullptr);
          if (atomname[0] == 'H' && atomname != bkbn_H_)
          {
            a_j++;
            continue;
          }
          std::size_t delta  = a_i - a_j;
          rvec sym_dx;
          if (pbc != nullptr) pbc_dx(pbc, x[ii], x[jj], sym_dx);
          else rvec_sub(x[ii], x[jj], sym_dx);
          float dx2 = iprod(sym_dx, sym_dx);
          if (i==j) 
          {
            if (dx2 < cut_sig_2_)
            { // intra molecule species
              f_intra_mol_(i, a_i, a_j, dx2, weight, mol_id_, natmol2_, density_bins_, inv_num_mol_, frame_same_mutex_, intram_mat_density_);
            }
          }
          else
          {
            if (mol_id_[i]==mol_id_[j])
            { // inter same molecule specie
              if (dx2 < cut_sig_2_)
              {
                f_inter_mol_same_(
                  i, mol_i, a_i, a_j, dx2, weight, mol_id_, natmol2_, density_bins_, frame_same_mutex_, frame_same_mat_, interm_same_mat_density_
                );
              }
              if (delta!=0.) {
                // this is to account for inversion atom/molecule
                if (pbc != nullptr) pbc_dx(pbc, x[ii-delta], x[jj+delta], sym_dx);
                else rvec_sub(x[ii-delta], x[jj+delta], sym_dx);
                dx2 = iprod(sym_dx, sym_dx);
                if (dx2 < cut_sig_2_)
                {
                  f_inter_mol_same_(
                    i, mol_i, a_i, a_j, dx2, weight, mol_id_, natmol2_, density_bins_, frame_same_mutex_, frame_same_mat_, interm_same_mat_density_
                  );
                }
              }
            } 
            else
            { // inter cross molecule species
              if (dx2 < cut_sig_2_)
              {
                f_inter_mol_cross_(
                  i, j, mol_i, mol_j, a_i, a_j, dx2, weight, mol_id_, natmol2_, cross_index_, density_bins_, frame_cross_mutex_, frame_cross_mat_, interm_cross_mat_density_
                );
              }
            }
          }
          ++a_j;
        }
        ++a_i;
      }
      ++mol_j;
    }
    semaphore_.release();
  }

public:
  CMData(
    const std::string &top_path, const std::string &traj_path,
    float cutoff, float mol_cutoff, int nskip, int num_threads, int num_mol_threads,
    int dt, const std::string &mode, const std::string &bkbn_H, const std::string &weights_path, 
    bool no_pbc, float t_begin, float t_end, bool h5
  ) : cutoff_(cutoff), mol_cutoff_(mol_cutoff), nskip_(nskip), dt_(dt), t_begin_(t_begin), t_end_(t_end),
      bkbn_H_(bkbn_H), weights_path_(weights_path), no_pbc_(no_pbc), num_threads_(num_threads),
      num_mol_threads_(num_mol_threads), mode_(mode), h5_(h5)
  {
    matrix boxtop_;
    mtop_ = (gmx_mtop_t*)malloc(sizeof(gmx_mtop_t));
    int natoms;
    pbcType_ = read_tpx(top_path.c_str(), nullptr, boxtop_, &natoms, nullptr, nullptr, mtop_);

    if (no_pbc_)
    {
      pbc_ = nullptr;
    }
    else
    {
      pbc_ = (t_pbc*)malloc(sizeof(t_pbc));
      set_pbc(pbc_, pbcType_, boxtop_);
    }

    int natom;
    long unsigned int nframe;
    int64_t *offsets;

    frame_ = (cmdata::xtc::Frame*)malloc(sizeof(cmdata::xtc::Frame));
    std::cout << "Reading trajectory file " << traj_path << std::endl;
    read_xtc_header(traj_path.c_str(), &natom, &nframe, &offsets);
    *frame_ = cmdata::xtc::Frame(natom);
    frame_->nframe = nframe;
    frame_->offsets = offsets;

    trj_ = xdrfile_open(traj_path.c_str(), "r");
    initAnalysis();
  }

  ~CMData()
  {
    free(frame_->x);
    free(frame_->offsets);
    free(frame_);
    free(mtop_);
    if (xcm_ != nullptr) free(xcm_);
  }

  void initAnalysis()
  /**
   * @brief Initializes the analysis by setting up the molecule partitioning and the mode selection
   * 
   * @todo Check if molecule block is empty
  */
  {
    n_x_ = 0;

    // get the number of atoms per molecule
    // equivalent to mols_ = gmx:gmx_mtop_molecules(*top.mtop());
    for (const gmx_molblock_t &molb : mtop_->molblock)
    {
      int natm_per_mol = mtop_->moltype[molb.type].atoms.nr;
      for (int i = 0; i < molb.nmol; i++) mols_.appendBlock(natm_per_mol);
      num_mol_unique_.push_back(molb.nmol);
    }
    // number of molecules
    nindex_ = mols_.numBlocks();

    if (num_threads_ > std::thread::hardware_concurrency())
    {
      num_threads_ = std::thread::hardware_concurrency();
      std::cout << "Maximum thread number surpassed. Scaling num_threads down to " << num_threads_ << std::endl;
    }
    if (num_mol_threads_ > std::thread::hardware_concurrency())
    {
      num_mol_threads_ = std::thread::hardware_concurrency();
      std::cout << "Maximum thread number surpassed. Scaling num_mol_threads down to " << num_mol_threads_ << std::endl;
    }
    if (num_mol_threads_ > nindex_)
    {
      num_mol_threads_ = nindex_;
      std::cout << "Number of molecule threads surpassed number of molecules. Setting num_mol_threads to " << num_mol_threads_ << std::endl;
    }
    threads_.resize(num_threads_);
    mol_threads_.resize(nindex_);
    semaphore_.set_counter(num_mol_threads_);
    std::cout << "Using " << num_threads_ << " threads and " << num_mol_threads_ << " molecule threads" << std::endl;

    // set up mode selection
    f_intra_mol_ = cmdata::ftypes::do_nothing<ftype_intra_>();
    f_inter_mol_same_ = cmdata::ftypes::do_nothing<ftype_same_>();
    f_inter_mol_cross_ = cmdata::ftypes::do_nothing<ftype_cross_>();

    printf("Evaluating mode selection:\n");
    std::string tmp_mode;
    std::stringstream modestream{ mode_ };
    while (std::getline(modestream, tmp_mode, '+'))
    {
      if (tmp_mode == std::string("intra"))
      {
        intra_ = true;
      }
      else if (tmp_mode == std::string("same"))
      {
        same_ = true;
      }
      else if (tmp_mode == std::string("cross"))
      {
        cross_ = true;
      }
      else
      {
        printf("Wrong mode: %s\nMode must be one from: intra, same, cross. Use + to concatenate more than one, i.e. intra+cross\n", tmp_mode.c_str());
        exit(1);
      }
      printf(" - found %s\n", tmp_mode.c_str());
    }

    int mol_id = 0;
    int molb_index = 0;
    for ( auto i : num_mol_unique_ )
    {
      natmol2_.push_back(mols_.block(molb_index).end() - mols_.block(molb_index).begin());
      inv_num_mol_unique_.push_back(1. / static_cast<float>(i));
      for ( int j = 0; j < i; j++ )
      {
        mol_id_.push_back(mol_id);
        inv_num_mol_.push_back(1. / static_cast<float>(i));
      }
      mol_id++;
      molb_index += i;
    }

    printf("Number of different molecules %lu\n", natmol2_.size());
    bool check_same = false;
    for(std::size_t i=0; i<natmol2_.size();i++) {
      printf("mol %lu num %u size %u\n", i, num_mol_unique_[i], natmol2_[i]);
      if(num_mol_unique_[i]>1) check_same = true;
    }
    if(!check_same && same_) same_ = false;
    if(natmol2_.size()<2) cross_ = false; 
    if(nindex_>1 && (same_ || cross_))
    {
      printf("\n\n::::::::::::WARNING::::::::::::\nMore than 1 molecule found in the system.\nFix pbc before running cmdata using pbc mol\n");
      printf(":::::::::::::::::::::::::::::::\n\n");
    }
    if (same_)
    {
      f_inter_mol_same_ = cmdata::density::inter_mol_same_routine;
      std::cout << ":: activating intermat same calculations" << std::endl;
      interm_same_mat_density_.resize(natmol2_.size());
      interm_same_maxcdf_mol_.resize(natmol2_.size());
    }
    if (cross_)
    {
      f_inter_mol_cross_ = cmdata::density::inter_mol_cross_routine;
      std::cout << ":: activating intermat cross calculations" << std::endl;
      interm_cross_mat_density_.resize((natmol2_.size() * (natmol2_.size() - 1)) / 2);
      interm_cross_maxcdf_mol_.resize((natmol2_.size() * (natmol2_.size() - 1)) / 2);
    }
    if (intra_) 
    {
      f_intra_mol_ = cmdata::density::intra_mol_routine;
      std::cout << " :: activating intramat calculations" << std::endl;
      intram_mat_density_.resize(natmol2_.size());
    }

    density_bins_.resize(cmdata::indexing::n_bins(cutoff_));
    for (std::size_t i = 0; i < density_bins_.size(); i++)
      density_bins_[i] = cutoff_ / static_cast<float>(density_bins_.size()) * static_cast<float>(i) + cutoff_ / static_cast<float>(density_bins_.size() * 2);

    int cross_count = 0;
    if (cross_) cross_index_.resize(natmol2_.size(), std::vector<int>(natmol2_.size(), 0));
    for ( std::size_t i = 0; i < natmol2_.size(); i++ )
    {
      if (same_)
      {
        interm_same_mat_density_[i].resize(natmol2_[i], std::vector<std::vector<float>>(natmol2_[i], std::vector<float>(cmdata::indexing::n_bins(cutoff_), 0)));
        interm_same_maxcdf_mol_[i].resize(natmol2_[i], std::vector<std::vector<float>>(natmol2_[i], std::vector<float>(cmdata::indexing::n_bins(cutoff_), 0)));
      }
      if (intra_) intram_mat_density_[i].resize(natmol2_[i], std::vector<std::vector<float>>(natmol2_[i], std::vector<float>(cmdata::indexing::n_bins(cutoff_), 0)));
      for ( std::size_t j = i + 1; j < natmol2_.size() && cross_; j++ )
      {
        interm_cross_mat_density_[cross_count].resize(natmol2_[i], std::vector<std::vector<float>>(natmol2_[j], std::vector<float>(cmdata::indexing::n_bins(cutoff_), 0)));
        interm_cross_maxcdf_mol_[cross_count].resize(natmol2_[i], std::vector<std::vector<float>>(natmol2_[j], std::vector<float>(cmdata::indexing::n_bins(cutoff_), 0)));
        cross_index_[i][j] = cross_count;
        cross_count++;
      }
    }

    n_bins_ = cmdata::indexing::n_bins(cutoff_);
    dx_ = cutoff_ / static_cast<float>(n_bins_);

    mcut2_ = mol_cutoff_ * mol_cutoff_;
    cut_sig_2_ = (cutoff_ + 0.02) * (cutoff_ + 0.02);
    xcm_ = (rvec*)malloc(nindex_ * sizeof(rvec));

    if (same_) frame_same_mat_.resize(natmol2_.size());
    if (intra_ || same_) frame_same_mutex_.resize(natmol2_.size());
    if (cross_) frame_cross_mat_.resize(cross_index_.size());
    if (cross_) frame_cross_mutex_.resize(cross_index_.size());
    for ( std::size_t i = 0; i < natmol2_.size(); i++ )
    {
      if (same_) frame_same_mat_[i].resize(natmol2_[i] *  natmol2_[i] * num_mol_unique_[i], 0);
      if (intra_ || same_) frame_same_mutex_[i] = std::vector<std::mutex>(natmol2_[i] *  natmol2_[i]);
      for ( std::size_t j = i+1; j < natmol2_.size() && cross_; j++ )
      {
        frame_cross_mat_[cross_index_[i][j]].resize(natmol2_[i] * natmol2_[j] * num_mol_unique_[i] * num_mol_unique_[j], 0);
        frame_cross_mutex_[cross_index_[i][j]] = std::vector<std::mutex>(natmol2_[i] * natmol2_[j]);
      }
    }

    if (weights_path_ != "")
    {
      printf("Weights file provided. Reading weights from %s\n", weights_path_.c_str());
      weights_ = cmdata::io::read_weights_file(weights_path_);
      printf("Found %li frame weights in file\n", weights_.size());
      float w_sum = std::accumulate(std::begin(weights_), std::end(weights_), 0.0, std::plus<>());
      printf("Sum of weights amounts to %lf\n", w_sum);
      weights_sum_ = 0.;
    }

    std::cout << "Calculating threading indices" << std::endl;
    /* calculate the mindist accumulation indices */
    std::size_t num_ops_same = 0;
    for ( std::size_t im = 0; im < natmol2_.size(); im++ ) num_ops_same += num_mol_unique_[im] * ( natmol2_[im] * ( natmol2_[im] + 1 ) ) / 2;
    int n_per_thread_same = (same_) ? num_ops_same / num_threads_  : 0;
    int n_threads_same_uneven = (same_) ? num_ops_same % num_threads_ : 0;
    std::size_t start_mti_same = 0, start_im_same = 0, end_mti_same = 1, end_im_same = 1; 
    std::size_t start_i_same = 0, start_j_same = 0, end_i_same = 0, end_j_same = 0;
    int num_ops_cross = 0;
    for ( std::size_t im = 0; im < natmol2_.size(); im++ )
    {
      for ( std::size_t jm = im + 1; jm < natmol2_.size(); jm++ )
      {
        num_ops_cross += num_mol_unique_[im] * natmol2_[im] * num_mol_unique_[jm] * natmol2_[jm];
      }
    }
    int n_per_thread_cross = (cross_) ? num_ops_cross / num_threads_ : 0;
    int n_threads_cross_uneven = (cross_) ? num_ops_cross % num_threads_ : 0;

    std::size_t start_mti_cross = 0, start_mtj_cross = 1, start_im_cross = 0, start_jm_cross = 0, start_i_cross = 0, start_j_cross = 0;
    std::size_t end_mti_cross = 1, end_mtj_cross = 2, end_im_cross = 1, end_jm_cross = 1, end_i_cross = 0, end_j_cross = 0;

    for ( int tid = 0; tid < num_threads_; tid++ )
    {
      /* calculate same indices */
      int n_loop_operations_same = n_per_thread_same + (tid < n_threads_same_uneven ? 1 : 0);
      long int n_loop_operations_total_same = n_loop_operations_same;
      while ( natmol2_[end_mti_same - 1] - static_cast<int>(end_j_same) <= n_loop_operations_same )
      {
        int sub_same = natmol2_[end_mti_same - 1] - static_cast<int>(end_j_same);
        n_loop_operations_same -= sub_same;
        end_i_same++;
        end_j_same = end_i_same;
        if (static_cast<int>(end_j_same) == natmol2_[end_mti_same - 1])
        {
          end_im_same++;
          end_i_same = 0;
          end_j_same = 0;
        }
        if (static_cast<int>(end_im_same) -1 == num_mol_unique_[end_mti_same - 1])
        {
          end_mti_same++;
          end_im_same = 1;
          end_i_same = 0;
          end_j_same = 0;
        }
        if (n_loop_operations_same == 0) break;
      }
      end_j_same += n_loop_operations_same;  
      /* calculate cross indices */
      int n_loop_operations_total_cross = n_per_thread_cross + ( tid < n_threads_cross_uneven ? 1 : 0 );
      if (natmol2_.size() > 1)
      {
        int n_loop_operations_cross = n_loop_operations_total_cross;
        while ( natmol2_[end_mti_cross-1] * natmol2_[end_mtj_cross-1] - (natmol2_[end_mtj_cross-1] * static_cast<int>(end_i_cross) + static_cast<int>(end_j_cross)) <= n_loop_operations_cross )
        {
          int sub_cross = natmol2_[end_mti_cross-1] * natmol2_[end_mtj_cross-1] - (natmol2_[end_mtj_cross-1] * static_cast<int>(end_i_cross) + static_cast<int>(end_j_cross));
          n_loop_operations_cross -= sub_cross;

          end_jm_cross++;
          end_i_cross = 0;
          end_j_cross = 0;

          // case jm is above max
          if (end_jm_cross > num_mol_unique_[end_mtj_cross - 1])
          {
            // end_im_cross++;
            end_mtj_cross++;
            end_jm_cross = 1;
            end_i_cross = 0;
            end_j_cross = 0;
          }
          if (end_mtj_cross > natmol2_.size())
          {
            end_im_cross++;
            end_mtj_cross = end_mti_cross + 1;
            end_jm_cross = 1;
            end_i_cross = 0;
            end_j_cross = 0;
          }
          if (end_im_cross > num_mol_unique_[end_mti_cross - 1])
          {
            end_mti_cross++;
            end_mtj_cross = end_mti_cross + 1;
            end_im_cross = 1;
            end_jm_cross = 1;
            end_i_cross = 0;
            end_j_cross = 0;
          }
          if (end_mti_cross == natmol2_.size()) break;
          if (n_loop_operations_cross == 0) break;
        }

        // calculate overhangs and add them
        if (end_mti_cross < natmol2_.size())
        {
          end_i_cross += n_loop_operations_cross / natmol2_[end_mtj_cross-1];
          end_j_cross += n_loop_operations_cross % natmol2_[end_mtj_cross-1];
          end_i_cross += end_j_cross / natmol2_[end_mtj_cross-1];
          end_j_cross %= natmol2_[end_mtj_cross-1];
        }
      }

      if (same_)
      {
        same_thread_indices_.push_back({
          start_mti_same, start_im_same, start_i_same, start_j_same, end_mti_same,
          end_im_same, end_i_same, end_j_same, n_loop_operations_total_same
        });
      }
      else
      {
        same_thread_indices_.push_back({
          0, 0, 0, 0, 0, 0, 0, 0, 0
        });
      }
      if (cross_ && natmol2_.size() > 1)
      {
        cross_thread_indices_.push_back({
          start_mti_cross, start_mtj_cross, start_im_cross, start_jm_cross, start_i_cross,
          start_j_cross, end_mti_cross, end_mtj_cross, end_im_cross, end_jm_cross, end_i_cross,
          end_j_cross, n_loop_operations_total_cross
        });
      }
      else
      {
        cross_thread_indices_.push_back({
          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        });
      }

      /* set new starts */
      start_mti_same = end_mti_same - 1;
      start_im_same = end_im_same - 1;
      start_i_same = end_i_same;
      start_j_same = end_j_same;

      start_mti_cross = end_mti_cross - 1;
      start_mtj_cross = end_mtj_cross - 1;
      start_im_cross = end_im_cross - 1;
      start_jm_cross = end_jm_cross - 1;
      start_i_cross = end_i_cross;
      start_j_cross = end_j_cross;
    }

    printf("Finished preprocessing.\nStarting frame-by-frame analysis.\n");
  }

  void run()
  {
    std::cout << "Running frame-by-frame analysis" << std::endl;
    int frnr = 0;
    float progress = 0.0, new_progress = 0.0;
    cmdata::io::print_progress_bar(progress);
    while (frame_->read_next_frame(trj_, no_pbc_, pbcType_, pbc_) == exdrOK)
    {
      new_progress = static_cast<float>(frnr) / static_cast<float>(frame_->nframe);
      if (new_progress - progress > 0.01)
      {
        progress = new_progress;
        cmdata::io::print_progress_bar(progress);
      }
      if ((frame_->time >= t_begin_ && (t_end_ < 0 || frame_->time <= t_end_ )) && // within time borders 
          ( dt_ == 0 || std::fmod(frame_->time, dt_) == 0) && (nskip_ == 0 || std::fmod(frnr, nskip_) == 0)) // skip frames
      {
        float weight = 1.0;
        if (!weights_.empty())
        {
          weight = weights_[frnr];
          weights_sum_ += weight;
        }
        /* resetting the per frame vector to zero */
        for ( std::size_t i = 0; i < frame_same_mat_.size(); i++ )
        {
          #pragma omp parallel for num_threads(std::min(num_threads_, static_cast<int>(frame_same_mat_[i].size())))
          for ( std::size_t j = 0; j < frame_same_mat_[i].size(); j++ ) frame_same_mat_[i][j] = 100.;
        }
        for ( std::size_t i = 0; i < frame_cross_mat_.size(); i++ )
        {
          #pragma omp parallel for num_threads(std::min(num_threads_, static_cast<int>(frame_cross_mat_[i].size())))
          for ( std::size_t j = 0; j < frame_cross_mat_[i].size(); j++ ) frame_cross_mat_[i][j] = 100.;
        }
        #pragma omp parallel for num_threads(std::min(num_threads_, nindex_))
        for (int i = 0; i < nindex_; i++)
        {
          clear_rvec(xcm_[i]);
          float tm = 0.;
          for (int ii = mols_.block(i).begin(); ii < mols_.block(i).end(); ii++)
          {
            for (int m = 0; (m < DIM); m++)
            {
              xcm_[i][m] += frame_->x[ii][m];
            }
            tm += 1.0;
          }
          for (int m = 0; (m < DIM); m++)
          {
            xcm_[i][m] /= tm;
          }
        }
        /* start loop for each molecule */
        for (int i = 0; i < nindex_; i++)
        {
          /* start molecule thread*/
          mol_threads_[i] = std::thread(molecule_routine, i, nindex_, pbc_, frame_->x, std::cref(inv_num_mol_), 
          cut_sig_2_, std::cref(natmol2_), std::cref(num_mol_unique_), std::cref(mol_id_), std::cref(cross_index_),
          std::cref(density_bins_), mcut2_, xcm_, mols_, mtop_, std::ref(frame_same_mat_),
          std::ref(frame_same_mutex_), std::ref(intram_mat_density_), std::ref(interm_same_mat_density_), std::ref(frame_cross_mat_), 
          std::ref(frame_cross_mutex_), std::ref(interm_cross_mat_density_), std::ref(semaphore_), std::cref(f_intra_mol_),
          std::cref(f_inter_mol_same_), std::cref(f_inter_mol_cross_), weight, bkbn_H_);
          /* end molecule thread */
        }
        /* join molecule threads */
        for ( auto &thread : mol_threads_ ) thread.join();

        /* calculate the mindist accumulation indices */
        for ( int tid = 0; tid < num_threads_; tid++ )
        {
          threads_[tid] = std::thread(
            cmdata::mindist::mindist_kernel, std::cref(same_thread_indices_[tid]), std::cref(cross_thread_indices_[tid]),
            weight, std::cref(natmol2_), std::cref(density_bins_), std::cref(num_mol_unique_),
            std::cref(frame_same_mat_), std::ref(frame_same_mutex_), std::ref(interm_same_maxcdf_mol_),
            std::cref(cross_index_), std::cref(frame_cross_mat_), std::ref(frame_cross_mutex_), std::ref(interm_cross_maxcdf_mol_)
          );
        }
        for ( auto &thread : threads_ ) thread.join();
        ++n_x_;
      }
      ++frnr;
    }

    cmdata::io::print_progress_bar(1.0);
  }

  void process_data()
  {
    std::cout << "\nFinished frame-by-frame analysis\n";
    std::cout << "Analyzed " << n_x_ << " frames\n";
    std::cout << "Normalizing data... " << std::endl;
    // normalisations
    float norm = ( weights_.empty() ) ? 1. / n_x_ : 1. / weights_sum_;

    using ftype_norm = cmdata::ftypes::function_traits<decltype(&cmdata::density::normalize_histo)>;
    std::function<ftype_norm::signature> f_empty = cmdata::ftypes::do_nothing<ftype_norm>();

    std::function<ftype_norm::signature> normalize_intra = (intra_) ? cmdata::density::normalize_histo : f_empty;
    std::function<ftype_norm::signature> normalize_inter_same = (same_) ? cmdata::density::normalize_histo : f_empty;
    std::function<ftype_norm::signature> normalize_inter_cross = (cross_) ? cmdata::density::normalize_histo : f_empty;

    for (std::size_t i = 0; i < natmol2_.size(); i++)
    {
      for (int ii = 0; ii < natmol2_[i]; ii++)
      {
        for (int jj = ii; jj < natmol2_[i]; jj++)
        {
          float inv_num_mol_same = inv_num_mol_unique_[i];
          normalize_inter_same(i, ii, jj, norm, inv_num_mol_same, interm_same_maxcdf_mol_);
          normalize_inter_same(i, ii, jj, norm, 1.0, interm_same_mat_density_);
          normalize_intra(i, ii, jj, norm, 1.0, intram_mat_density_);

          float sum = 0.0;
          for ( std::size_t k = (same_) ? 0 : cmdata::indexing::n_bins(cutoff_); k < cmdata::indexing::n_bins(cutoff_); k++ )
          {
            sum+= dx_ * interm_same_maxcdf_mol_[i][ii][jj][k];
            if (sum > 1.0) sum=1.0;
            interm_same_maxcdf_mol_[i][ii][jj][k] = sum;
          }
          if (same_) interm_same_mat_density_[i][jj][ii] = interm_same_mat_density_[i][ii][jj];
          if (same_) interm_same_maxcdf_mol_[i][jj][ii] = interm_same_maxcdf_mol_[i][ii][jj];
          if (intra_) intram_mat_density_[i][jj][ii] = intram_mat_density_[i][ii][jj];
        }
      }
      for (std::size_t j = i + 1; j < natmol2_.size() && cross_; j++)
      {
        for (int ii = 0; ii < natmol2_[i]; ii++)
        {
          for (int jj = 0; jj < natmol2_[j]; jj++)
          {
            float inv_num_mol_cross = inv_num_mol_unique_[i];
            normalize_inter_cross(cross_index_[i][j], ii, jj, norm, 1.0, interm_cross_mat_density_);
            normalize_inter_cross(cross_index_[i][j], ii, jj, norm, inv_num_mol_cross, interm_cross_maxcdf_mol_);

            float sum = 0.0;
            for ( std::size_t k = (cross_) ? 0 : cmdata::indexing::n_bins(cutoff_); k < cmdata::indexing::n_bins(cutoff_); k++ )
            {
              sum += dx_ * interm_cross_maxcdf_mol_[cross_index_[i][j]][ii][jj][k];
              if (sum > 1.0) sum = 1.0;
              interm_cross_maxcdf_mol_[cross_index_[i][j]][ii][jj][k] = sum;
            }
          }
        }
      }
    }
  }
  
  void write_output( const std::string &output_prefix )
  {
    std::cout << "Writing data... " << std::endl;
    using ftype_write_intra = cmdata::ftypes::function_traits<decltype(&cmdata::io::f_write_intra)>;
    using ftype_write_inter_same = cmdata::ftypes::function_traits<decltype(&cmdata::io::f_write_inter_same)>;
    using ftype_write_inter_cross = cmdata::ftypes::function_traits<decltype(&cmdata::io::f_write_inter_cross)>;
    std::function<ftype_write_intra::signature> write_intra = cmdata::ftypes::do_nothing<ftype_write_intra>();
    std::function<ftype_write_inter_same::signature> write_inter_same = cmdata::ftypes::do_nothing<ftype_write_inter_same>();
    std::function<ftype_write_inter_cross::signature> write_inter_cross = cmdata::ftypes::do_nothing<ftype_write_inter_cross>();

    if (intra_)
    {
      #ifdef USE_HDF5
      if(h5_) write_intra = cmdata::io::f_write_intra_HDF5;
      else write_intra = cmdata::io::f_write_intra;
      #else
      write_intra = cmdata::io::f_write_intra;
      #endif
    }
    if (same_) 
    {
      #ifdef USE_HDF5
      if(h5_) write_inter_same = cmdata::io::f_write_inter_same_HDF5;
      else write_inter_same = cmdata::io::f_write_inter_same;
      #else
      write_inter_same = cmdata::io::f_write_inter_same;
      #endif
    }
    if (cross_)
    {
      #ifdef USE_HDF5
      if(h5_) write_inter_cross = cmdata::io::f_write_inter_cross_HDF5;
      else write_inter_cross = cmdata::io::f_write_inter_cross;
      #else
      write_inter_cross = cmdata::io::f_write_inter_cross;
      #endif
    }

    for (std::size_t i = 0; i < natmol2_.size(); i++)
    {
      std::cout << "Writing data for molecule " << i << "..." << std::endl;
      cmdata::io::print_progress_bar(0.0);
      float progress = 0.0, new_progress = 0.0;

      for (int ii = 0; ii < natmol2_[i]; ii++)
      {
        new_progress = static_cast<float>(ii) / static_cast<float>(natmol2_[i]);
        if (new_progress - progress > 0.01)
        {
          progress = new_progress;
          cmdata::io::print_progress_bar(progress);
        }
        write_intra(output_prefix, i, ii, density_bins_, natmol2_, intram_mat_density_);
        write_inter_same(output_prefix, i, ii, density_bins_, natmol2_, interm_same_mat_density_, interm_same_maxcdf_mol_);
      }
      if(cross_)
      {
        for (std::size_t j = i + 1; j < natmol2_.size(); j++)
        {
          for (int ii = 0; ii < natmol2_[i]; ii++)
          {
            write_inter_cross(output_prefix, i, j, ii, density_bins_, natmol2_, cross_index_, interm_cross_mat_density_, interm_cross_maxcdf_mol_);
          }
        }
      }
    }

    cmdata::io::print_progress_bar(1.0);
    std::cout << "\nFinished!" << std::endl;
  }
};


} // namespace cmdata

#endif // _CM_DATA_HPP
