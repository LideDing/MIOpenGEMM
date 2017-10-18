/*******************************************************************************
 * Copyright (C) 2017 Advanced Micro Devices, Inc. All rights reserved.
 *******************************************************************************/
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <tuple>
#include <miopengemm/derivedparams.hpp>
#include <miopengemm/error.hpp>
#include <miopengemm/macgrid.hpp>
#include <miopengemm/tiling.hpp>

namespace MIOpenGEMM
{

std::string ChiralDerivedParams::get_string()
{
  std::stringstream ss;
  ss << "\nmacro_tile_length : " << macro_tile_length
     << "\nn_elements_in_unroll : " << n_elements_in_unroll
     << "\nmain_n_elements_to_load_per_workitem : " << main_n_elements_to_load_per_workitem
     << "\nmain_n_elements_in_padded_unroll : " << main_n_elements_in_padded_unroll
     << "\nmain_n_micro_tiles_pll_unroll : " << main_n_micro_tiles_pll_unroll
     << "\nmain_macro_tile_length_and_pad : " << main_macro_tile_length_and_pad
     << "\nmain_n_micro_in_macro : " << main_n_micro_in_macro
     << "\npreshift_final_tile : " << preshift_final_tile << "\nn_groups : " << n_groups;

  return ss.str();
}

std::string DerivedParams::get_string()
{
  std::stringstream ss;
  for (auto x : {Mat::E::A, Mat::E::B})
  {
    ss << "\n" << Mat::M().name[x] << "\n" << at(x).get_string();
  }
  return ss.str();
}

size_t DerivedParams::get_target_ld(Mat::E emat_x) const { return at(emat_x).cw1_target_ldx; }


//get_target(16, get_copy_pad(emat_x), at(emat_x).cw1_smallest_possible_ldx);
size_t get_target(size_t grid_size, size_t above_distance, size_t x)
{
  size_t to_grid_line = (x - above_distance) / grid_size + ((x - above_distance) % grid_size != 0);

  return grid_size * to_grid_line + above_distance;
}

size_t get_copy_pad(Mat::E emat_x)
{
  if (emat_x == Mat::E::A)
  {
    return 3;
  }
  else
  {
    return 6;
  }
}

void DerivedParams::reset_cw_params(Mat::E emat_x)
{
  if (emat_x == Mat::E::B && ptr_hp->sus[Mat::E::A].vs[Chi::E::WOS] != Scratch::E::UNUSED &&
      adps.cw_n_elements == uninitialised_size_t)
  {
    throw miog_error("make sure reset acw1 params is called before reset_bcw1_params, we need that"
                     " adps.cw1_target_ldx be set here in derivedparams reset of bcw1");
  }

  // simple copy with padding
  if (ptr_hp->sus[emat_x].vs[Chi::E::WOS] == Scratch::E::COPY)
  {
    at(emat_x).cw1_smallest_possible_ldx =
      ptr_gg->coal_is_pll_k(emat_x) ? ptr_gg->k : ptr_gg->get_non_k_dim(emat_x);
    at(emat_x).cw1_target_ldx =
      get_target(16, get_copy_pad(emat_x), at(emat_x).cw1_smallest_possible_ldx);
    at(emat_x).cw_n_elements = at(emat_x).cw1_target_ldx * ptr_gg->get_uncoal(emat_x);
  }

  else if (ptr_hp->sus[emat_x].vs[Chi::E::WOS] == Scratch::E::NFORM)
  {

    at(emat_x).cw2_n_elements_perp_unroll = at(emat_x).n_groups * at(emat_x).macro_tile_length;
    at(emat_x).cw_n_elements              = at(emat_x).cw2_n_elements_perp_unroll * ptr_gg->k;

    cw2_n_macro_tiles_pll_unroll = ptr_gg->k / ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR] +
                                   ((ptr_gg->k % ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR]) != 0);
  }

  else
  {
    std::stringstream errm;
    errm << "In [" << Mat::M().name[emat_x] << "] [" << Chi::M().name[Chi::E::WOS] << "] . ";
    errm << " Input is " << ptr_hp->sus[emat_x].vs[Chi::E::WOS] << " . ";
    errm << " It should be 1 or 2 in reset_cw_params ";
    throw miog_error(errm.str());
  }
}

Derivabilty::Derivabilty(const HyPas& hp, const Geometry& gg)
{
  DerivedParams dp(hp, gg, "uninitialised");
  auto          tup = dp.set_fragile();
  is_derivable      = std::get<0>(tup);
  msg               = std::get<1>(tup);
}

bool is_dvble(const HyPas& hp, const Geometry& gg)
{
  Derivabilty dble(hp, gg);
  return dble.is_derivable;
}

DerivedParams::DerivedParams(const HyPas& hp_, const Geometry& gg_, std::string s)
  : ptr_hp(&hp_), ptr_gg(&gg_)
{
  if (s.compare("uninitialised") != 0)
  {
    throw miog_error("the only string with which a DerivedParams object can be "
                     "initialised is `uninitialised'");
  }
}

std::tuple<bool, std::string> DerivedParams::set_fragile()
{

  set_should_be_hyperparams();

  macgrid::Grid grid(ptr_hp->sus[Mat::E::C].vs[NonChi::E::MAC],
                     ptr_hp->sus[Mat::E::C].vs[NonChi::E::SKW]);
  if (!grid.is_good)
  {
    return std::make_tuple(false, grid.error_message);
  }

  at(Mat::E::A).macro_tile_length = grid.at(Mat::E::A) * ptr_hp->sus[Mat::E::A].vs[Chi::E::MIC];
  at(Mat::E::B).macro_tile_length = grid.at(Mat::E::B) * ptr_hp->sus[Mat::E::B].vs[Chi::E::MIC];

  for (auto emat_x : {Mat::E::A, Mat::E::B})
  {
    at(emat_x).preshift_final_tile =
      1 + (ptr_gg->get_non_k_dim(emat_x) - 1) % at(emat_x).macro_tile_length;
    at(emat_x).n_groups = ptr_gg->get_non_k_dim(emat_x) / at(emat_x).macro_tile_length +
                          (at(emat_x).preshift_final_tile != at(emat_x).macro_tile_length);
    at(emat_x).main_macro_tile_length_and_pad =
      at(emat_x).macro_tile_length +
      ptr_hp->sus[emat_x].vs[Chi::E::VEW] * ptr_hp->sus[emat_x].vs[Chi::E::PAD];

    at(emat_x).main_n_elements_in_padded_unroll =
      at(emat_x).main_macro_tile_length_and_pad * ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR];
  }

  main_macro_tile_area = at(Mat::E::B).macro_tile_length * at(Mat::E::A).macro_tile_length;
  main_micro_tile_area =
    ptr_hp->sus[Mat::E::B].vs[Chi::E::MIC] * ptr_hp->sus[Mat::E::A].vs[Chi::E::MIC];
  main_n_work_items_per_workgroup = main_macro_tile_area / main_micro_tile_area;

  required_workspaces = {};

  std::stringstream set_status_ss;

  for (auto emat_x : {Mat::E::A, Mat::E::B})
  {
    // check - 3 : the macro tile is too tall
    if (ptr_gg->m < at(Mat::E::A).macro_tile_length)
    {

      set_status_ss << "ptr_hp->sus[Mat::E::C].vs[NonChi::E::MAC] = "
                    << ptr_hp->sus[Mat::E::C].vs[NonChi::E::MAC]
                    << "  ptr_hp->sus[Mat::E::C].vs[NonChi::E::SKW]  = "
                    << ptr_hp->sus[Mat::E::C].vs[NonChi::E::SKW]
                    << "  grid.at(Mat::E::A) = " << grid.at(Mat::E::A)
                    << "  grid.at(Mat::E::B) = " << grid.at(Mat::E::B) << '\n';

      set_status_ss << "ptr_gg->m = " << ptr_gg->m
                    << " and at(Mat::E::A).macro_tile_length = " << at(Mat::E::A).macro_tile_length
                    << " . ";
      set_status_ss << "m < at(Mat::E::A).macro_tile_length, not considering this kernel. ";
    }

    // check - 4 : the macro tile is too wide
    else if (ptr_gg->n < at(Mat::E::B).macro_tile_length)
    {
      set_status_ss << "n < at(Mat::E::B).macro_tile_length, not considering this kernel. ";
    }

    at(emat_x).n_elements_in_unroll =
      at(emat_x).macro_tile_length * ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR];
    at(emat_x).main_n_elements_to_load_per_workitem =
      at(emat_x).n_elements_in_unroll / main_n_work_items_per_workgroup;

    if (ptr_hp->sus[Mat::E::C].vs[NonChi::E::STR] == 0 && ptr_hp->sus[emat_x].vs[Chi::E::WOS] == Scratch::E::NFORM)
    {
      at(emat_x).cw2_n_elements_to_load_per_workitem =
        at(emat_x).n_elements_in_unroll / at(emat_x).cw2_local_work_size;
    }

    if (ptr_hp->sus[Mat::E::C].vs[NonChi::E::STR] == 0 && ptr_hp->sus[emat_x].vs[Chi::E::WOS] != Scratch::E::UNUSED)
    {
      reset_cw_params(emat_x);
      required_workspaces.push_back({at(emat_x).cw_n_elements,
                                     emat_x,
                                     static_cast<Scratch::E>(ptr_hp->sus[emat_x].vs[Chi::E::WOS])});
    }

    // check 0 : macro tile not too large
    if (ptr_gg->get_non_k_dim(emat_x) < at(emat_x).macro_tile_length)
    {
      set_status_ss << "ptr_gg->get_non_k_dim( " << Mat::M().name[emat_x] << " )  < at ( "
                    << Mat::M().name[emat_x]
                    << " ).macro_tile_length, this means the tile is too big "
                       "to work with  "
                    << Mat::M().name[emat_x] << " . not considering this kernel. ";
    }
  }

  std::sort(required_workspaces.rbegin(), required_workspaces.rend());
  // here we assume wSpaceSize is sorted in descending order too.

  // check -1 : enough workspace memory
  if (required_workspaces.size() > ptr_gg->wSpaceSize.size())
  {
    set_status_ss
      << "required_workspaces vector is longer than wSpaceSize : too few workspace memories. ";
  }

  else
  {
    for (size_t i = 0; i < required_workspaces.size(); ++i)
    {
      if (ptr_gg->wSpaceSize[i] < required_workspaces[i].n_elms)
      {
        set_status_ss << "The [" << i << "] largest workspace in geometry is "
                      << ptr_gg->wSpaceSize[i] << ", but the [" << i << "] largest required is "
                      << required_workspaces[i].n_elms << '.';
      }
    }
  }

  if (set_status_ss.str() != "")
  {
    return std::make_tuple(false, set_status_ss.str());
  }

  // check 1 : n_work_items_per_workgroup divides n_elements_in_unroll for a and b  */

  auto is_div = [&set_status_ss, this](Mat::E emat_x, std::string which, size_t val) {

    if (at(emat_x).n_elements_in_unroll % val != 0)
    {
      set_status_ss << "this is not supported: " << which << " (" << val
                    << ") is not a factor of n_elements_in_(" << Mat::M().name[emat_x]
                    << ")_unroll (" << at(emat_x).n_elements_in_unroll << "). \n"
                    << "Consider rounding unroll up. ";
      return std::make_tuple<bool, std::string>(false, set_status_ss.str());
    }
    else
    {
      return std::make_tuple<bool, std::string>(true, {});
    }
  };

  for (auto emat_x : {Mat::E::A, Mat::E::B})
  {

    auto tup = is_div(emat_x, "main_n_work_items_per_workgroup", main_n_work_items_per_workgroup);
    if (std::get<0>(tup) == false)
    {
      return tup;
    }

    if (ptr_hp->sus[emat_x].vs[Chi::E::WOS] == Scratch::E::NFORM)
    {
      auto tup_cw =
        is_div(emat_x, "at(emat_x).cw2_local_work_size", at(emat_x).cw2_local_work_size);
      if (std::get<0>(tup_cw) == false)
      {
        return tup_cw;
      }
    }
  }

  // check 2 : tileability
  for (auto emat_x : {Mat::E::A, Mat::E::B})
  {

    auto tup = tiling::get_tileability(at(emat_x).macro_tile_length,
                                       ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR],
                                       at(emat_x).main_n_elements_to_load_per_workitem);
    if (std::get<0>(tup) == false)
    {
      return tup;
    }

    if (ptr_hp->sus[emat_x].vs[Chi::E::WOS] == Scratch::E::NFORM)
    {
      tup = tiling::get_tileability(at(emat_x).macro_tile_length,
                                    ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR],
                                    at(emat_x).cw2_n_elements_to_load_per_workitem);
      if (std::get<0>(tup) == false)
      {
        return tup;
      }
    }
  }

  if (ptr_hp->sus[Mat::E::C].vs[NonChi::E::UFO] == Binary::E::YES)
  {
    if (ptr_gg->k <= ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR])
    {
      return std::make_tuple(false, "UFO = yes, so UNR must be greater that k");
    }
  }

  if (ptr_hp->sus[Mat::E::C].vs[NonChi::E::UFO] == Binary::E::YES &&
      (ptr_hp->sus[Mat::E::A].vs[Chi::E::LOM] == Binary::E::NO ||
       ptr_hp->sus[Mat::E::B].vs[Chi::E::LOM] == Binary::E::NO))
  {
    return std::make_tuple(false, "UFO = yes, so both LOMs must be YES");
  }

  main_split_on_k      = ptr_hp->sus[Mat::E::C].vs[NonChi::E::ICE] == 1 ? 0 : 1;
  main_does_beta_c_inc = main_split_on_k == 1 ? 0 : 1;

  if (ptr_hp->sus[Mat::E::C].vs[NonChi::E::GAL] == 3)
  {
    if (main_split_on_k == 1)
    {
      ga3_super_column_width = static_cast<size_t>(
        std::floor(std::sqrt(static_cast<double>(ptr_hp->sus[Mat::E::C].vs[NonChi::E::NAW]) /
                             static_cast<double>(ptr_hp->sus[Mat::E::C].vs[NonChi::E::ICE]))));
    }
    else if (main_split_on_k == 0)
    {
      ga3_super_column_width = static_cast<size_t>(
        std::floor(std::sqrt(static_cast<double>(ptr_hp->sus[Mat::E::C].vs[NonChi::E::NAW]))));
    }
    else
    {
      throw miog_error("main_split_on_k is neither 0 nor 1, how can this be? Logic error");
    }

    if (ga3_super_column_width == 0)
    {
      return std::make_tuple(false, "ga3_super_column_width would be 0 ( ICE >  NAW) ");
    }

    ga3_last_super_column_width = bdps.n_groups % ga3_super_column_width;
  }

  // do the tiling
  for (auto emat_x : {Mat::E::A, Mat::E::B})
  {

    tiling::set_tile_dimensions(at(emat_x).main_micro_tile_perp_unroll,
                                at(emat_x).main_micro_tile_pll_unroll,
                                at(emat_x).macro_tile_length,
                                ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR],
                                at(emat_x).main_n_elements_to_load_per_workitem,
                                ptr_hp->sus[emat_x].vs[Chi::E::PLU] == 0);

    if (ptr_hp->sus[emat_x].vs[Chi::E::WOS] == Scratch::E::NFORM)
    {
      tiling::set_tile_dimensions(at(emat_x).cw2_micro_tile_perp_unroll,
                                  at(emat_x).cw2_micro_tile_pll_unroll,
                                  at(emat_x).macro_tile_length,
                                  ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR],
                                  at(emat_x).cw2_n_elements_to_load_per_workitem,
                                  at(emat_x).cw2_load_pll_to_unroll == 0);
    }
  }

  for (auto emat_x : {Mat::E::A, Mat::E::B})
  {

    at(emat_x).main_n_micro_in_macro =
      at(emat_x).macro_tile_length / ptr_hp->sus[emat_x].vs[Chi::E::MIC];
    at(emat_x).main_n_micro_tiles_pll_unroll =
      ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR] / at(emat_x).main_micro_tile_pll_unroll;
    at(emat_x).main_c_interweave_stride =
      ptr_hp->sus[emat_x].vs[Chi::E::MIW] == 0 ? 1 : at(emat_x).main_n_micro_in_macro;

    if (ptr_hp->sus[emat_x].vs[Chi::E::WOS] == Scratch::E::NFORM)
    {
      at(emat_x).cw2_n_micro_tiles_pll_unroll =
        ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR] / at(emat_x).cw2_micro_tile_pll_unroll;
      at(emat_x).cw2_n_micro_tiles_perp_unroll =
        at(emat_x).macro_tile_length / at(emat_x).cw2_micro_tile_perp_unroll;
    }
  }

  // check vector-izability
  std::stringstream ss_viz;
  bool              is_viz = true;
  for (auto emat_x : {Mat::E::A, Mat::E::B})
  {
    if (ptr_hp->sus[emat_x].vs[Chi::E::VEW] != 1)
    {
      if (get_stride(emat_x, false, false, static_cast<Scratch::E>(ptr_hp->sus[emat_x].vs[Chi::E::WOS])) != 1)
      {
        ss_viz << "stride perp to k of " << Mat::M().name[emat_x] << " is not 1.\n";
        is_viz = false;
      }

      if (at(emat_x).main_micro_tile_perp_unroll % ptr_hp->sus[emat_x].vs[Chi::E::VEW] != 0)
      {
        ss_viz << "micro load tile perp to unroll of " << Mat::M().name[emat_x] << " ( "
               << at(emat_x).main_micro_tile_perp_unroll << " )  is not divisable by VEW.\n";
        is_viz = false;
      }

      if (ptr_hp->sus[emat_x].vs[Chi::E::MIC] % ptr_hp->sus[emat_x].vs[Chi::E::VEW] != 0)
      {

        ss_viz << "micro tile dim-" << Mat::M().name[emat_x] << " ( "
               << ptr_hp->sus[emat_x].vs[Chi::E::MIC] << " )  is not divisable by VEW.\n";
        is_viz = false;
      }

      // check if load stride is divisible by vector width.
      auto one_stride_pll_k  = get_stride(emat_x, true, false, static_cast<Scratch::E>(ptr_hp->sus[emat_x].vs[Chi::E::WOS]));
      auto load_stride_pll_k = (ptr_hp->sus[emat_x].vs[Chi::E::LIW] == Binary::E::YES)
                                 ? one_stride_pll_k * at(emat_x).main_n_micro_tiles_pll_unroll
                                 : one_stride_pll_k;

      if (load_stride_pll_k % ptr_hp->sus[emat_x].vs[Chi::E::VEW] != 0)
      {
        ss_viz << "load-stride-pll-k for dim-" << Mat::M().name[emat_x] << " ( "
               << load_stride_pll_k << " )  is not divisable by VEW. "
               << "main_n_micro_tiles_pll_unroll = " << at(emat_x).main_n_micro_tiles_pll_unroll
               << "  one-stride-pll-k : " << one_stride_pll_k;
        is_viz = false;
      }
    }
  }

  std::string viza = ss_viz.str();

  if (!is_viz)
  {
    return std::make_tuple(false, viza);
  }

  bool run_ROCm_test = false;
  if (run_ROCm_test == true)
  {
    // see rocm.cpp in tests. Note, there is a timeout in OCL if other cases exists.
    for (auto emat : {Mat::E::A, Mat::E::B})
    {
      if (ptr_hp->sus[Mat::E::C].vs[NonChi::E::MAC] != 1 &&
          grid.at(emat) == ptr_hp->sus[Mat::E::C].vs[NonChi::E::MAC])
      {
        std::stringstream errm;
        errm << "ROCm 1.6 compiler specific case : extreme grid stretch (1xMAX) and "
             << Mat::M().name[emat] << " has PLU1_LIW0.";
        return std::make_tuple(false, errm.str());
      }
    }
  }


  // strassen initialisation. 
  
  
  for (auto emx : {Mat::E::A, Mat::E::B})
  {
    at(emx).strassen.subw_coal = ptr_gg->get_coal(emx)/2 + ptr_gg->get_coal(emx)%2 != 0;
    at(emx).strassen.subw_uncoal = ptr_gg->get_uncoal(emx)/2 + ptr_gg->get_uncoal(emx)%2 != 0;
    at(emx).strassen.ldw = get_target(at(emx).strassen.workspace_grid, at(emx).strassen.workspace_grid_offset, at(emx).strassen.subw_coal);
    at(emx).strassen.inter_subw_stride = at(emx).strassen.ldw * at(emx).strassen.subw_uncoal;
    at(emx).strassen.threads_per_subw = at(emx).strassen.inter_subw_stride / at(emx).strassen.work_per_thread;
    at(emx).strassen.n_work_iterms_per_line = at(emx).strassen.ldw / at(emx).strassen.work_per_thread;
    at(emx).strassen.total_nthreads = at(emx).strassen.n_subw * at(emx).strassen.threads_per_subw;
  
    //TODO : confirm that ldw is divided by work_per_thread
    //TODO : if vectorised, confirm that gg.get_coal(emat_x) is divided by work_per_thread
    //TODO : confirm ldw >= subw_coal
  }
  stras_m = ptr_gg->m / 2 + ptr_gg->m%2 != 0;
  stras_n = ptr_gg->n / 2 + ptr_gg->n%2 != 0;
  stras_k = ptr_gg->n / 2 + ptr_gg->n%2 != 0;

  // ran the gauntlet, returning deriveable is true
  return std::make_tuple(true, "");
}

std::string get_tint(size_t memsize)
{

  std::string tint;
  if (memsize < std::pow(2, 16))
  {  // 2 bytes = 16 bits.
    tint = "ushort";
  }

  else if (memsize < std::pow(2, 32))
  {  // 4 bytes = 32 bits.
    tint = "unsigned";
  }

  else
  {
    tint = "size_t";
  }

  return tint;
}

DerivedParams::DerivedParams(const HyPas& hp_, const Geometry& gg_) : ptr_hp(&hp_), ptr_gg(&gg_)
{

  auto tup = set_fragile();

  if (std::get<0>(tup) == false)
  {
    throw miog_error("Failure to construct DerivedParams. Problem caught in set_fragile. It "
                     "is "
                     "recommended to run function ` derivable ' to check that a valid "
                     "DerivedParams can be constructed. The message returned in set_fragile "
                     "is : "
                     " " +
                     std::get<1>(tup));
  }

  if (ptr_hp->sus[Mat::E::C].vs[NonChi::E::ICE] == 1)
  {
    infa = "n_work_items_per_c_elm is 1, should not be using atomics";
    fati = "n_work_items_per_c_elm is 1, should not be using atomics";
  }

  else
  {
    infa = ptr_gg->derived.float_size_bits == 32 ? "uint" : "ulong";
    fati = ptr_gg->derived.float_size_bits == 32 ? "atomic_cmpxchg" : "atom_cmpxchg";
  }

  pragma_unroll_string = ptr_hp->sus[Mat::E::C].vs[NonChi::E::PUN] == 1 ? "#pragma unroll\n" : "";

  kstring = ptr_hp->sus[Mat::E::C].vs[NonChi::E::PAK] == Binary::E::YES ? "k" : "KVAL__";

  effective_k_varies_string =
    ptr_hp->sus[Mat::E::C].vs[NonChi::E::UFO] == 0 ? kstring : "k_plus_offset";
  t_float = ptr_gg->derived.float_size_bits == 32 ? "float" : "double";

  k_effective_mod_G_UNROLL = effective_k_varies_string + " % G_UNROLL";
  k_effective_div_G_UNROLL = effective_k_varies_string + " / G_UNROLL";
  k_effective_div_UNROLL   = effective_k_varies_string + " / UNROLL";

  main_n_work_groups = ptr_hp->sus[Mat::E::C].vs[NonChi::E::ICE] *
                       ((ptr_gg->m / at(Mat::E::A).macro_tile_length) +
                        (ptr_gg->m % at(Mat::E::A).macro_tile_length != 0)) *
                       ((ptr_gg->n / at(Mat::E::B).macro_tile_length) +
                        (ptr_gg->n % at(Mat::E::B).macro_tile_length != 0));

  main_global_work_size = main_n_work_groups * main_n_work_items_per_workgroup;

  main_use_edge_trick = (ptr_gg->m % at(Mat::E::A).macro_tile_length == 0 &&
                         ptr_gg->n % at(Mat::E::B).macro_tile_length == 0)
                          ? 0
                          : 1;
  main_final_fractional_unroll = (ptr_hp->sus[Mat::E::C].vs[NonChi::E::UFO] == 1 ||
                                  ptr_gg->k % ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR] != 0)
                                   ? 1
                                   : 0;

  tints[Mat::E::A] = get_tint(ptr_gg->get_uncoal(Mat::E::A) *
                              (ptr_gg->ldX[Mat::E::A]));  // TODO : does UFO need increase here ?
  tints[Mat::E::B] = get_tint(ptr_gg->get_uncoal(Mat::E::B) * (ptr_gg->ldX[Mat::E::B]));
  tints[Mat::E::C] = get_tint(ptr_gg->get_uncoal(Mat::E::C) * (ptr_gg->ldX[Mat::E::C]));
  tints_vws.resize(ptr_gg->wSpaceSize.size());
  for (auto workspace_index = 0; workspace_index < tints_vws.size(); ++workspace_index)
  {
    tints_vws[workspace_index] = get_tint(ptr_gg->wSpaceSize[workspace_index]);
  }

  tintk = get_tint(
    ptr_gg->k +
    2 * ptr_hp->sus[Mat::E::C].vs[NonChi::E::ICE] *
      ptr_hp->sus[Mat::E::C].vs[NonChi::E::UNR]);  // TODO : make this tight and prove correct.

  if (ptr_hp->sus[Mat::E::C].vs[NonChi::E::SZT] == true)
  {
    std::string ui64 = "ulong";
    tints[Mat::E::A] = ui64;
    tints[Mat::E::B] = ui64;
    tints[Mat::E::C] = ui64;
    for (auto& x : tints_vws)
    {
      x = ui64;
    }
    tintk = ui64;
  }

  tshort = "ushort";
}

/* TODO : move to hyper params */
void DerivedParams::set_should_be_hyperparams()
{

  betac_local_work_size = 256;
  betac_work_per_thread = 2;

  for (auto emat_x : {Mat::E::A, Mat::E::B})
  {

    at(emat_x).cw1_local_work_size    = 256;
    at(emat_x).cw1_work_per_thread    = 2;
    at(emat_x).cw2_load_pll_to_unroll = 0;
    at(emat_x).cw2_local_work_size    = 64;
  }
}

size_t
DerivedParams::get_stride(Mat::E emat_x, bool pll_k, bool is_macro, Scratch::E wst) const
{

  if (wst == Scratch::E::UNUSED)
  {
    return get_stride_ws_unused(emat_x, pll_k);
  }

  else if (wst == Scratch::E::COPY)
  {
    return get_stride_ws_copy(emat_x, pll_k);
  }

  else if (wst == Scratch::E::NFORM)
  {
    return get_stride_ws_nform(emat_x, pll_k, is_macro);
  }
  else if (wst == Scratch::E::STR_A125x67){
    return at(Mat::E::A).strassen.ldw;
  }
  
  else if (wst == Scratch::E::STR_B34 || wst == Scratch::E::STR_B167){
    return at(Mat::E::B).strassen.ldw;
  }
  
  else
    throw miog_error("unrecognised workspace_type in get_strinde in derivedparams");
}

size_t DerivedParams::get_stride_ws_unused(Mat::E emat_x, bool pll_k) const
{
  return ptr_gg->coal_is_pll_k(emat_x) == pll_k ? 1 : ptr_gg->ldX.at(emat_x);
}

size_t DerivedParams::get_stride_ws_copy(Mat::E emat_x, bool pll_k) const
{
  return ptr_gg->coal_is_pll_k(emat_x) == pll_k ? 1 : at(emat_x).cw1_target_ldx;
}

size_t DerivedParams::get_stride_ws_nform(Mat::E emat_x, bool pll_k, bool is_macro) const
{
  if (is_macro == false)
  {
    return pll_k == true ? at(emat_x).macro_tile_length : 1;
  }
  else
  {
    return pll_k == true ? at(emat_x).macro_tile_length : ptr_gg->k;
  }
}

int DerivedParams::get_workspace_id(Mat::E emat, Scratch::E scratch) const
{
  for (int workspace_id = 0; workspace_id < required_workspaces.size(); ++workspace_id)
  {
    const auto& triple = required_workspaces[workspace_id];
    if (triple.emat == emat && triple.scratch == scratch)
    {
      return workspace_id;
    }
  }
  std::stringstream errms;
  errms << "failed to match request in get_workspace_id : " << Mat::M().name[emat] << ','
        << Scratch::M().name[scratch];
  throw miog_error(errms.str());
}

//}
}
