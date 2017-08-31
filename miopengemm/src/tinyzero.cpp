/*******************************************************************************
 * Copyright (C) 2017 Advanced Micro Devices, Inc. All rights reserved.
 *******************************************************************************/
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>
#include <miopengemm/architests.hpp>
#include <miopengemm/bundle.hpp>
#include <miopengemm/derivedparams.hpp>
#include <miopengemm/error.hpp>
#include <miopengemm/findparams.hpp>
#include <miopengemm/generic.hpp>
#include <miopengemm/graph.hpp>
#include <miopengemm/kernel.hpp>
#include <miopengemm/kernelcache.hpp>
#include <miopengemm/kernelstring.hpp>
#include <miopengemm/nearest.hpp>
#include <miopengemm/oclutil.hpp>
#include <miopengemm/outputwriter.hpp>
#include <miopengemm/redirection.hpp>
#include <miopengemm/solution.hpp>
#include <miopengemm/stringutilbase.hpp>
#include <miopengemm/timer.hpp>
#include <miopengemm/tinyzero.hpp>

// TODO : checks on constraints to check for cleary non-derivables
// TODO : checks on workspace size

namespace MIOpenGEMM
{

void   FindTracker::start() { timer.start(); }
double FindTracker::get_elapsed() const { return timer.get_elapsed(); }

void FindTracker::incr_descents() { ++descents; }
void FindTracker::incr_kernels() { ++kernels; }

size_t FindTracker::get_descents() const { return descents; }

std::string FindTracker::get_string() const
{
  auto format = [](const size_t& x) { return std::string("") + stringutil::get_padded(x, 7); };
  std::stringstream              track_ss;
  track_ss << "[ELAPSED[s]:" << format(static_cast<int>(timer.get_elapsed()))
           << "  #RESTARTS:" << format(descents) << "  #GEMMS:" << format(kernels) << "]       ";
  return track_ss.str();
}

GpuMms::GpuMms(cl_mem           a_gpu_,
               cl_mem           b_gpu_,
               cl_mem           c_gpu_,
               bool             c_is_const,
               cl_mem           workspace_gpu_,
               size_t           c_nbytes,
               cl_command_queue cq)
{

  cl_mems[Mem::E::A] = a_gpu_;
  cl_mems[Mem::E::B] = b_gpu_;
  cl_mems[Mem::E::W] = workspace_gpu_;

  if (c_is_const == false)
  {
    cl_mems[Mem::E::C] = c_gpu_;
  }

  else
  {
    cl_mems[Mem::E::C] =
      oclutil::get_copy(cq, c_gpu_, c_nbytes, "c_is_const is true, making copy in GpuMms");
    c_copy.clmem = cl_mems[Mem::E::C];  // for correct destruction
  }
}

cl_mem& GpuMms::operator[](Mem::E x) { return cl_mems[x]; }

TinyZero::TinyZero(cl_command_queue command_queue_,
                   const Geometry   gg_,
                   const Offsets    toff_,
                   cl_mem           a_gpu_,
                   cl_mem           b_gpu_,
                   cl_mem           c_gpu_,
                   bool             c_is_const,
                   cl_mem           workspace_gpu_,
                   owrite::Writer&  mowri_)
  : command_queue(command_queue_),
    gg(gg_),
    toff(toff_),
    gpum(a_gpu_,
         b_gpu_,
         c_gpu_,
         c_is_const,
         workspace_gpu_,
         get_mat_memsize(gg, toff, Mat::E::C),
         command_queue_),
    devinfo(command_queue_),
    mowri(mowri_)
{
  cl_context   context;
  cl_device_id device_id;
  oclutil::cl_set_context_and_device_from_command_queue(
    command_queue, context, device_id, mowri, true);

  for (size_t i = 0; i < KType::E::N; ++i)
  {
    tk_kernels[i] = Kernel(device_id, context, &tk_events[i], KType::M.name[i]);
  }
}

void TinyZero::address_check_valid()
{
  for (auto x : {Mem::E::A, Mem::E::B})
  {
    if (gpum[Mem::E::C] == gpum[x])
    {
      std::stringstream ss;
      ss << "in address_check_valid, " << Mem::M.name[Mem::E::C] << " and " << Mem::M.name[x]
         << " should have distinct memories, "
         << "otherwise race condition arise (one thread writes its result to "
         << Mem::M.name[Mem::E::C] << "before another one has finished reading from "
         << Mem::M.name[Mem::E::C] << ')';
      throw miog_error(ss.str());
    }
  }

  if (gpum[Mem::E::C] == nullptr)
  {
    throw miog_error("in address_check_valid, c should not be nullptr");
  }

  if (gpum[Mem::E::W] == nullptr && gg.wSpaceSize != 0)
  {
    throw miog_error("in address_check_valid, pointer to workspace memory is "
                     "the nullptr, but wSpaceSize is not zero");
  }

  if (gpum[Mem::E::W] != nullptr && gg.wSpaceSize == 0)
  {
    throw miog_error("in address_check_valid, pointer to workspace memory is not the "
                     "nullptr, but wSpaceSize is zero. if wSpaceSize is zero please set "
                     "workspace_gpu to the nullptr to make super clear that there will be "
                     "no workspace used. The workspace offset should be zero too in this case ");
  }

  if (gpum[Mem::E::W] != nullptr &&
      (gpum[Mem::E::W] == gpum[Mem::E::A] || gpum[Mem::E::W] == gpum[Mem::E::B] ||
       gpum[Mem::E::W] == gpum[Mem::E::C]))
  {
    throw miog_error("in address_check_valid, pointer to workspace memory is "
                     "not the nullptr, and it is the same as one of the a,b,c pointers ");
  }
}

void TinyZero::address_check_valid_and_reliable()
{
  address_check_valid();
  if (gpum[Mem::E::A] == gpum[Mem::E::B])
  {
    throw miog_error("in address_check_valid_and_reliable, a and b are the same. this will "
                     "effect kernel run time, not sure if this should be allowed, so throwing");
  }
}

void TinyZero::set_kern_args(const KernBlob& kblob)
{
  auto arg_sizes_values = kerngen::get_arg_sizes_values(kblob,
                                                        gpum.cl_mems,
                                                        toff.offsets,
                                                        gg.derived.float_size_bytes,
                                                        Floating::m_alpha[gg.floattype],
                                                        Floating::m_beta[gg.floattype]);

  tk_kernels.at(kblob.e_ktype).set_kernel_args(arg_sizes_values);
}

void TinyZero::setup_tinykernels(const kerngen::Bundle& bundle)
{

  oclutil::Result oclr;

  // TODO setting v_wait_indices here : is not clear code
  v_wait_indices = kerngen::get_v_wait_indices(bundle.v_tgks, mowri);

  tk_kernels_active.resize(0);

  for (size_t ksi = 0; ksi < bundle.v_tgks.size(); ++ksi)
  {
    const KernBlob& kblob = bundle.v_tgks[ksi];
    if (tk_kernels.at(kblob.e_ktype).update_needed(kblob))
    {
      oclr = tk_kernels.at(kblob.e_ktype).update(kblob, mowri);
      if (oclr.fail() == false)
      {
        set_kern_args(kblob);
      }
      else
      {
        std::stringstream ss;
        // Maybe related : https://github.com/BVLC/caffe/issues/5610  (?)
        ss << "Failed in setup tinykernels. " << bundle.hp.get_string()
           << " Message: " << oclr.message << "Error status : " << oclr.success
           << " Bailing, please report if possible. ";
        throw miog_error(ss.str());
      }
    }
    tk_kernels_active.push_back(&tk_kernels[bundle.v_tgks[ksi].e_ktype]);
  }
}

std::string TinyZero::get_run_times_heading()
{
  std::stringstream ss;
  ss << "tt: \t";
  for (size_t k_ind = 0; k_ind < tk_kernels_active.size(); ++k_ind)
  {
    ss << " k" << k_ind << ":\t";
  }
  ss << "sum: \t Gflops/s:\n";
  return ss.str();
}

std::string TinyZero::get_run_time_string(cl_int status, double extime)
{
  std::stringstream ss;
  if (status == CL_SUCCESS)
  {
    ss << std::fixed << std::setprecision(3) << extime << '\t';

    double sumtimes = 0;

    for (size_t k_ind = 0; k_ind < tk_kernels_active.size(); ++k_ind)
    {
      auto tk = tk_kernels_active[k_ind]->v_times.back();
      sumtimes += tk;
      ss << " " << tk << "\t";
    }
    ss << std::fixed << std::setprecision(3) << sumtimes << '\t';
    ss << " " << 2.0 * gg.m * gg.n * gg.k / (extime * 1e6) << std::setprecision(6);
  }

  else
  {
    ss << "(failed run)";
  }
  return ss.str();
}

oclutil::Result TinyZero::true_core(std::function<void(std::string)> acton,
                                    std::vector<double>&             all_times,
                                    const Halt&                      hl)
{

  size_t          runi{0};
  oclutil::Result oclr;

  Timer timer;
  timer.start();

  all_times.resize(0);

  while (!hl.halt(runi, timer.get_elapsed()))
  {
    // see `overheat' comment at bottom

    if (tk_kernels_active.size() == 0)
    {

      throw miog_error("zero kernels active : internal logic error");
    }

    oclr = run_kernels(
      command_queue, tk_kernels_active, v_wait_indices, 0, nullptr);  //, false, nullptr);

    if (oclr.success == CL_SUCCESS)
    {
      // good
    }

    else if (oclr.success == CL_OUT_OF_RESOURCES)
    {
      oclutil::cl_flush(command_queue, "cl flushing in core gemm loop", true);
      oclr.message += " (CL_OUT_OF_RESOURCES in true_core) ";
      return oclr;
    }

    else
    {
      std::stringstream ss;
      ss << "OpenCL error status : " << oclr.success << ". "
         << "Neither CL_SUCCESS nor CL_OUT_OF_RESOURCES.  "
         << "Maybe there are no kernels? Internal logic error. "
         << "could catch with CL_OUT_OF_RESOURCES (ie throw oclr) "
         << "The error from opencl was " << oclr.message;
      throw miog_error(ss.str());
    }

    oclutil::cl_flush(command_queue, "cl flush in core gemm loop", true);

    // Wait for kernels to complete
    oclutil::cl_wait_for_events(1, tk_kernels_active.back()->ptr_event, "core gemm loops", true);

    size_t maxend   = 0;
    size_t minstart = std::numeric_limits<size_t>::max();
    for (auto& ptr_tk_kernel : tk_kernels_active)
    {
      ptr_tk_kernel->update_times();
      maxend   = std::max<size_t>(maxend, ptr_tk_kernel->t_end);
      minstart = std::min<size_t>(minstart, ptr_tk_kernel->t_start);
    }

    double extime = (1e-6 * (maxend - minstart));

    // act on the results string.
    acton(get_run_time_string(oclr.success, extime));
    ++runi;
    all_times.push_back(extime);
  }

  auto   best_time = *std::min_element(all_times.begin(), all_times.end());
  double gflops    = gg.get_gflops(best_time);
  mowri.bw[OutPart::BEN] << gg.get_tabbed_string()
                         << "  time[ms]:" << stringutil::get_char_padded(best_time, 10)
                         << "  gflops:" << gflops << Endl;

  return {};
}

std::vector<double> TinyZero::benchgemm(const HyPas& hp, const Halt& hl)
{

  address_check_valid();
  Derivabilty dblt(hp, gg);
  if (dblt.is_derivable == false)
  {
    throw miog_error("Non-derivable in benchgemm : " + dblt.msg);
  }

  kerngen::Bundle bundle(hp, gg);

  architests::Stat atr(command_queue, bundle.dp, gg, hp);
  if (!atr.is_good)
  {
    throw miog_error(atr.msg);
  }

  setup_tinykernels(bundle);

  mowri << "hyper-p   :" << hp.get_string() << '\n'
        << "geometry  :" << gg.get_string() << '\n'
        << "Entering the core gemm loops" << Endl;

  mowri << get_run_times_heading();

  std::vector<double> all_times;
  true_core([this](std::string x) { mowri << x << '\n'; }, all_times, hl);
  return all_times;
}

Solution TinyZero::find0(const Constraints& constraints, const FindParams& fparms)
{

  if (fparms.hl_outer.max_time < 0.01)
  {
    size_t rank = 0;
    mowri << "Time allotted to find is less that 0.01, so returning a default immediately.\n";
    return get_default(command_queue, gg, constraints, mowri, IfNoCache::E::GENERIC, rank);
  }

  address_check_valid_and_reliable();

  FindTracker ftrack;
  ftrack.start();
  std::vector<Solution> v_solns;

  bool   warmstart      = true;
  size_t warmstart_rank = 0;

  while (!fparms.hl_outer.halt(ftrack.get_descents(), ftrack.get_elapsed()))
  {
    mowri << "\nEntering new descent. \n"
          << fparms.hl_outer.get_status(ftrack.get_descents(), ftrack.get_elapsed()) << '\n';

    // 0, 1, 5, 10, 15, etc
    warmstart = (ftrack.get_descents() < 2 || ftrack.get_descents() % 5 == 0) ? true : false;

    double allotted_sd = std::max(0.1, fparms.hl_outer.max_time - ftrack.get_elapsed());

    auto soln = single_descent_find(
      allotted_sd, constraints, fparms.hl_core, ftrack, fparms.sumstat, warmstart, warmstart_rank);
    v_solns.emplace_back(soln);
    ftrack.incr_descents();

    if (warmstart)
    {
      ++warmstart_rank;
    }
  }

  double              best_gflops     = 0;
  size_t              best_soln_index = 0;
  std::vector<double> soln_gflops;
  for (size_t si = 0; si < v_solns.size(); ++si)
  {

    double gflops = gg.get_gflops(v_solns[si].extime);
    soln_gflops.push_back(gflops);
    if (gflops > best_gflops)
    {
      best_gflops     = gflops;
      best_soln_index = si;
    }
  }

  mowri << '\n'
        << "Search summary  :  " << ftrack.get_string() << '\n'
        << stringutil::get_star_wrapped("The gflops found by single descents:") << '\n'
        << '\n';

  std::sort(soln_gflops.begin(), soln_gflops.end());
  for (auto& x : soln_gflops)
  {
    mowri << x << "  ";
  }
  mowri << "\n\n";

  mowri.bw[OutPart::CCH] << "\n\n\n -- snip -- -- -- snip --\n\n" << Endl;

  bool is_not_canonical = redirection::get_is_not_canonical(gg);
  mowri.bw[OutPart::CCH] << get_cache_entry_string(
    {devinfo.device_name, constraints, gg}, v_solns[best_soln_index].hypas, is_not_canonical);
  mowri.bw[OutPart::CCH] << "\n -- snip -- -- -- snip --\n\n\n" << Endl;

  return v_solns[best_soln_index];
}

Solution TinyZero::single_descent_find(double             allotted_time,
                                       const Constraints& constraints,
                                       const Halt&        core_halt,
                                       FindTracker&       ftrack,
                                       SummStat::E        sumstat,
                                       bool               warmstart,
                                       size_t             warmstart_rank)
{

  // only considered an improvement if ratio new/old less than this
  double improvement_factor_required = 0.999;

  Timer timer;
  timer.start();

  mowri << "geometry : " << gg.get_string() << "\nallotted time : " << allotted_time << Endl;

  // re-creating the same graph for every single_descent is currently wasteful,
  // but maybe in the future different constraints will be passed on each run
  const Graph graph(gg, devinfo, constraints, mowri);

  // number of kernels whose strings are generated
  size_t single_descent_counter = 0;

  // We will store all previously considered HyPas, used to check and
  // ensure that we do not consider a HyperParam more than once
  // Maybe this should be in the outer find loop ?
  // Although then the stats between runs wouldn't be indep.
  std::vector<HyPas> hyper_front_history;

  // Keep track of the `records' as they get broken
  std::vector<Solution> best_solns_path;
  std::vector<double>   disco_times;

  // used for tracker messages
  std::string old_track_msg;
  std::string new_track_msg;

  std::vector<double> v_t_total;
  double              k_seconds;

  // the hyper params to be considered on a single wave
  std::vector<HyPas> hyper_front;

  HyPas warm_start_hp;
  if (warmstart == false)
  {
    // what if I put 2 or three here ? might help fast escape from bad region
    hyper_front = {graph.get_random_valid_start()};
  }
  else
  {
    mowri << "Warmstart requested [@ rank " << warmstart_rank << "]  " << Flush;
    auto soln =
      get_default(command_queue, gg, constraints, mowri, IfNoCache::E::RANDOM, warmstart_rank);
    warm_start_hp = soln.hypas;
    hyper_front   = {warm_start_hp};
  }

  HyPas hp_curr;

  bool improvement_found_on_front = true;

  while (improvement_found_on_front == true)
  {
    improvement_found_on_front = false;
    size_t hfi                 = 0;

    while (hfi < hyper_front.size() && improvement_found_on_front == false &&
           timer.get_elapsed() < allotted_time)
    {

      hp_curr = hyper_front[hfi];

      hyper_front_history.push_back(hp_curr);

      // extra precaution, should be able to remove this
      Derivabilty dblt(hp_curr, gg);
      if (dblt.is_derivable == false)
      {
        std::stringstream errm;
        errm << "Non-derivable in single descent find : " << dblt.msg << ".\n";
        errm << "Geometry: " << gg.get_string() << '\n';
        errm << "hp: " << hp_curr.get_string() << '\n';
        throw miog_error(errm.str());
      }

      kerngen::Bundle bundle(hp_curr, gg);
      // the OpenCL string was succesfully generated,
      // we can now attempt to compile and benchmark it
      ++single_descent_counter;

      mowri << "\n[" << single_descent_counter << ", " << std::fixed << std::setprecision(2)
            << timer.get_elapsed() << std::setprecision(6) << "s]\t" << hp_curr.get_string()
            << Endl;

      architests::Stat atr(command_queue, bundle.dp, gg, hp_curr);
      if (atr.is_good == false)
      {
        mowri << "architest failed: " << atr.msg << Endl;
        ++hfi;
        continue;
      }

      // kernel compilation
      setup_tinykernels(bundle);

      old_track_msg = new_track_msg;
      new_track_msg = ftrack.get_string();
      mowri.bw[OutPart::E::TRA] << std::string(old_track_msg.size(), '\b');
      mowri.bw[OutPart::E::TRA] << new_track_msg << Flush;

      v_t_total.resize(0);
      for (auto& ptr_tk_kernel : tk_kernels_active)
      {
        ptr_tk_kernel->reset_times();
      }
      std::vector<std::string> summary;

      auto oclr = true_core(
        [&summary, &v_t_total](std::string x) { summary.push_back(x); }, v_t_total, core_halt);

      if (oclr.fail())
      {
        mowri << "cl out of resources: " << atr.msg << Endl;
        ++hfi;
        continue;
      }

      auto v_t_total_copy = v_t_total;

      std::sort(v_t_total_copy.begin(), v_t_total_copy.end());
      switch (sumstat)
      {
      case SummStat::E::MAX: k_seconds    = v_t_total_copy[0]; break;
      case SummStat::E::MEDIAN: k_seconds = v_t_total_copy[v_t_total.size() / 2]; break;
      case SummStat::E::MEAN:
        k_seconds = std::accumulate(v_t_total.begin(), v_t_total.end(), 0.) / v_t_total.size();
        break;
      default: throw miog_error("unrecgnised SummStat in find");
      }

      mowri << get_run_times_heading();
      for (size_t ir = 0; ir < summary.size(); ++ir)
      {
        mowri << summary[ir];
        if (v_t_total[ir] == k_seconds)
        {
          mowri << " (" << SummStat::M.name[sumstat] << ')';
          if (best_solns_path.size() > 0 &&
              (improvement_factor_required * best_solns_path.back().extime >= k_seconds))
          {
            mowri << " (NEW BEST) ";
          }
        }
        mowri << '\n';
      }

      if (best_solns_path.size() == 0 ||
          (improvement_factor_required * best_solns_path.back().extime >= k_seconds))
      {

        improvement_found_on_front = true;

        best_solns_path.emplace_back(gg, k_seconds, bundle.v_tgks, hp_curr, devinfo, constraints);
        disco_times.push_back(timer.get_elapsed());
      }

      ++hfi;
      ftrack.incr_kernels();
    }

    if (improvement_found_on_front == true && allotted_time > timer.get_elapsed())
    {
      bool prioritize = single_descent_counter < 20;
      auto neighbors  = graph.get_neighbors(hp_curr, prioritize);

      // refreshing hyper front
      hyper_front.clear();

      for (auto& hp : neighbors)
      {
        if (std::count(neighbors.begin(), neighbors.end(), hp) > 1)
        {
          throw miog_error("duplicates in neighbors not allowed, should have already been "
                           "filtered. Could filter out here, but less efficient ");
        }

        else if (graph.contains(hp) == false)
        {
          std::stringstream errmss;
          errmss << "constraint violators not allowed, should have already been filtered out."
                 << "Could filter out here, but less efficient. The hyperstring is\n"
                 << hp.get_string();
          throw miog_error(errmss.str());
        }

        // filtering out if it has already been considered
        else if (std::find(hyper_front_history.begin(), hyper_front_history.end(), hp) !=
                 hyper_front_history.end())
        {
        }

        // filtering out non-deriveables
        else if (!is_dvble(hp, gg))
        {
        }

        // looks ok, adding it to the hyper-front
        else
        {
          hyper_front.push_back(hp);
        }
      }

      if (warmstart == true)
      {
        hyper_front.push_back(warm_start_hp);  // slipping the pernicious hp on the back.
      }
    }
  }

  mowri.bw[OutPart::E::TRA] << std::string(new_track_msg.size(), '\b') << Flush;

  if (timer.get_elapsed() >= allotted_time)
  {
    mowri << "stopping the search because allotted time has been surpassed" << Endl;
  }

  else if (improvement_found_on_front == false)
  {
    mowri << "stopping the search because a locally minimal kernel has been found" << Endl;
  }

  else
  {
    throw miog_error("why did the algorithm stop ? ");
  }

  if (best_solns_path.size() == 0)
  {
    throw miog_error("\nThere were no solutions found. This suggests that the initial kernel did "
                     "not work (could not derive hyper parameters, required too much memory, or "
                     "did not compile. Maybe there is some preceding warning printed which sheds "
                     "light on this? Probably with a modification to the FindStartType or the "
                     "constraints, this should be resolved. For example, the unroll UNR "
                     "can be reduced if the problem is memory. jn should catch certain problems "
                     "in architests ");
  }

  auto leading_size = best_solns_path.back().hypas.get_string().size() + 2;

  std::string startstring = "hyper parameter string:";
  startstring.resize(leading_size, ' ');
  mowri << '\n'
        << startstring << "\t time when found:\t " << SummStat::M.lcase_name[sumstat]
        << " gflops:" << Endl;

  for (unsigned i = 0; i < best_solns_path.size(); ++i)
  {
    std::string solnstring = best_solns_path[i].hypas.get_string();
    solnstring.resize(leading_size, ' ');
    mowri << std::fixed << solnstring << "\t " << disco_times[i] << "\t\t "
          << gg.get_gflops(best_solns_path[i].extime/1000.) << Endl;
  }

  return best_solns_path.back();
}
}

// comment `overheat'
// This pause should have zero effect but
// mysteriously it smooths out the run times
// between runs when working with certain
// drivers, something to do with overheating

// comment `cl-events'
// copying cl_events is dangerous.
// I have seen that copying them before passed to enqueue
// (last parameter) causes problems,
// this is my idea of what is going on, to confirm:
// from cl.h, we see that
// typedef struct _cl_event *          cl_event,
// that is cl_event is a pointer to a _cl_event.
// when a cl_event address is passed to enqueue,
// the value of it changes. that is it points to a different _cl_event.
// thus ev_a = ev_b, enqueue(..., ev_b)
// leaves ev_a pointing to the wrong (old) place
// checking the event is safe:
// clGetEventInfo takes cl_events by value.
// So the moral of the story is :
// don't copy cl_events before passing their address
// as non-const pointers somewhere!
// paruse cl.h, sometimes *cl_event is passed as const, sometimes not

// comment `in-series'
// if (k_ind == 0){ status = tk_kernels_active[k_ind]->enqueue();}
// else{ status = tk_kernels_active[k_ind]->enqueue(1, &(tk_kernels_active[k_ind -1]->clevent)); }