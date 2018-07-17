/**
 * Copyright (c) 2017. The WRENCH Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */


#include <wms/WMS.h>
#include <workflow/job/PilotJob.h>
#include <logging/TerminalOutput.h>
#include <managers/JobManager.h>
#include <StaticClusteringAlgorithms/ClusteredJob.h>
#include <StaticClusteringAlgorithms/StaticClusteringWMS.h>
#include "LevelByLevelWMS.h"
#include "OngoingLevel.h"
#include "PlaceHolderJob.h"

XBT_LOG_NEW_DEFAULT_CATEGORY(level_by_level_clustering_wms, "Log category for Level-by-Level Clustering WMS");


#define EXECUTION_TIME_FUDGE_FACTOR 1.1

namespace wrench {

    LevelByLevelWMS::LevelByLevelWMS(std::string hostname, bool overlap, std::string clustering_spec,
                                     BatchService *batch_service) :
            WMS(nullptr, nullptr, {batch_service}, {}, {}, nullptr, hostname, "clustering_wms") {
      this->overlap = overlap;
      this->batch_service = batch_service;
      this->clustering_spec = clustering_spec;
    }


    int LevelByLevelWMS::main() {

      TerminalOutput::setThisProcessLoggingColor(TerminalOutput::COLOR_CYAN);

      this->checkDeferredStart();

      // Find out core speed on the batch service
      this->core_speed = *(this->batch_service->getCoreFlopRate().begin());
      // Find out #hosts on the batch service
      this->number_of_hosts = this->batch_service->getNumHosts();

      // Create a job manager
      this->job_manager = this->createJobManager();


      while (not this->getWorkflow()->isDone()) {

        submitPilotJobsForNextLevel();

        this->waitForAndProcessNextEvent();

      }

      return 0;
    }


    void LevelByLevelWMS::submitPilotJobsForNextLevel() {

      WRENCH_INFO("Seeing if I can submit jobs for the 'next' level...");

      // If more than 2 levels are going on, forget it
      if (this->ongoing_levels.size() >= 2) {
        WRENCH_INFO("Too many ongoing levels going on... will try later");
        return;
      }

      // Don't schedule a pilot job if overlap = false and anything is going on
      if ((not this->overlap) and (not ongoing_levels.empty())) {
        return;
      }

      // Compute which level should be submitted
      unsigned long level_to_submit = ULONG_MAX;
      for (auto l : this->ongoing_levels) {
        unsigned long level_number = l.second->level_number;
        if ((level_to_submit == ULONG_MAX) or (level_to_submit < level_number)) {
          level_to_submit = level_number;
        }
      }
      level_to_submit++;

      if (level_to_submit >= this->getWorkflow()->getNumLevels()) {
        WRENCH_INFO("All workflow levels have been submitted!");
        return;
      }

      // Make sure that all PH jobs in the previous level have started
      if (level_to_submit > 0) {
        if (not (this->ongoing_levels[level_to_submit - 1]->pending_placeholder_jobs.empty())) {
          WRENCH_INFO("Cannot submit pilot jobs for level %ld since level %ld still has "
                              "pilot jobs that haven't started yet", level_to_submit, level_to_submit-1);
          return;
        }
      }

      if (level_to_submit > 0) {
        WRENCH_INFO("All pilot jobs from level %ld have started... off I go with level %ld!",
                    level_to_submit - 1, level_to_submit);
      } else {
        WRENCH_INFO("Starting the first level!");
      }

      WRENCH_INFO("Creating a new ongoing level for level %lu", level_to_submit);
      OngoingLevel *new_ongoing_level = new OngoingLevel();
      new_ongoing_level->level_number = level_to_submit;

      // Create all place holder jobs for level
      std::set<PlaceHolderJob *> place_holder_jobs;
      place_holder_jobs = createPlaceHolderJobsForLevel(level_to_submit);

      // Submit placeholder jobs
      for (auto ph : place_holder_jobs) {
        new_ongoing_level->pending_placeholder_jobs.insert(ph);

        // Create the pilot job
        double makespan = ph->clustered_job->estimateMakespan(this->core_speed);
        // Create the pilot job
        ph->pilot_job = this->job_manager->createPilotJob(
                ph->clustered_job->getNumNodes(),
                1, 0, makespan * EXECUTION_TIME_FUDGE_FACTOR);


        // submit the corresponding pilot job
        std::map<std::string, std::string> service_specific_args;
        service_specific_args["-N"] = std::to_string(ph->pilot_job->getNumHosts());
        service_specific_args["-c"] = std::to_string(ph->pilot_job->getNumCoresPerHost());
        service_specific_args["-t"] = std::to_string(1 + ((unsigned long) (ph->pilot_job->getDuration())) / 60);
        this->job_manager->submitJob(ph->pilot_job, this->batch_service,
                                     service_specific_args);
        WRENCH_INFO("Submitted a Pilot Job (%s hosts, %s min) for workflow level %lu (%s)",
                    service_specific_args["-N"].c_str(),
                    service_specific_args["-t"].c_str(),
                    level_to_submit,
                    ph->pilot_job->getName().c_str());
        WRENCH_INFO("This pilot job has these tasks:");
        for (auto t : ph->clustered_job->getTasks()) {
          WRENCH_INFO("     - %s (flops: %lf)", t->getID().c_str(), t->getFlops());
        }
      }

      ongoing_levels.insert(std::make_pair(level_to_submit, new_ongoing_level));
    }



    std::set<PlaceHolderJob *> LevelByLevelWMS::createPlaceHolderJobsForLevel(unsigned long level) {

      /** Identify relevant tasks **/
      std::vector<WorkflowTask *> tasks;

      std::vector<WorkflowTask *> tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(level,level);

      for (auto t : tasks_in_level) {
        if (t->getState() != WorkflowTask::COMPLETED) {
          tasks.push_back(t);
        }
      }

      /** Apply Clustering heuristics **/
      std::istringstream ss(this->clustering_spec);
      std::string token;
      std::vector<std::string> tokens;

      while(std::getline(ss, token, '-')) {
        tokens.push_back(token);
      }

      /** Invoke task clustering heuristics **/
      std::set<ClusteredJob *>clustered_jobs;
      if (tokens[0] == "hc") {
        if (tokens.size() != 3) {
          throw std::runtime_error("createPlaceHolderJobsForLevel(): Invalid clustering spec " + this->clustering_spec);
        }
        unsigned long num_tasks_per_cluster;
        unsigned long num_nodes_per_cluster;
        if ((sscanf(tokens[1].c_str(), "%lu", &num_tasks_per_cluster) != 1) or (num_tasks_per_cluster < 1) or
            (sscanf(tokens[2].c_str(), "%lu", &num_nodes_per_cluster) != 1) or (num_nodes_per_cluster < 1)) {
          throw std::invalid_argument("Invalid static:hc specification");
        }
        // Compute clusters (could be 0 nodes, in which case queue prediction will be triggered)
        clustered_jobs = StaticClusteringWMS::createHCJobs(
                "none", num_tasks_per_cluster, num_nodes_per_cluster,
                this->getWorkflow(), level, level);

      } else {
        throw std::runtime_error("createPlaceHolderJobsForLevel(): Invalid clustering spec " + this->clustering_spec);
      }


      /** Transform clustered jobs into PlaceHolderJobs */
      std::set<PlaceHolderJob *> place_holder_jobs;
      for (auto cj : clustered_jobs) {

        // Create the placeholder job (with a for-now nullptr PilotJob)
        WRENCH_INFO("Creating a placeholder job for level %ld based on a clustered job with %ld tasks",
                    level, cj->getNumTasks());
        PlaceHolderJob *ph = new PlaceHolderJob(nullptr, cj, level, level);

        // Add it to the set
        place_holder_jobs.insert(ph);
      }

      return place_holder_jobs;
    }


//
//
//      // Queue-based num nodes
//      for (auto cj : clustered_jobs) {
//        // Construct
//        unsigned long max_num_nodes = std::min(cj->getNumTasks(), this->number_of_hosts);
//        std::string job_id = "my_tentative_job";
//        std::set<std::tuple<std::string,unsigned int,unsigned int, double>> set_of_jobs;
//        for (unsigned int n = 1; n < max_num_nodes; n++) {
//          double walltime_seconds = 1000;
//          std::tuple<std::string, unsigned int, unsigned int, double> my_job =
//                  std::make_tuple(job_id + "_" + std::to_string(sequence_number++),
//                                  n, 1, walltime_seconds);
//          set_of_jobs.insert(my_job);
//        }
//
//          std::map<std::string,double> jobs_estimated_start_times;
//          bool success = true;
//          try {
//            jobs_estimated_start_times = batch_service->getStartTimeEstimates(set_of_jobs);
//          } catch (wrench::WorkflowExecutionException &e) {
//          }
//        }
//
//      }



    void LevelByLevelWMS::processEventPilotJobStart(std::unique_ptr<PilotJobStartedEvent> e) {
      // Just for kicks, check it was the pending one
      WRENCH_INFO("Got a Pilot Job Start event: %s", e->pilot_job->getName().c_str());

      // Find the placeholder job in the pending list
      PlaceHolderJob *placeholder_job = nullptr;
      OngoingLevel *ongoing_level = nullptr;

      for (auto ol : this->ongoing_levels) {
        for (auto ph : ol.second->pending_placeholder_jobs) {
          if (ph->pilot_job == e->pilot_job) {
            placeholder_job = ph;
            ongoing_level = ol.second;
            break;
          }
        }
      }

      if (placeholder_job == nullptr) {
        throw std::runtime_error("Fatal Error: couldn't find a placeholder job for a pilot job that just started");
      }

      WRENCH_INFO("The corresponding placeholder job has %ld tasks", placeholder_job->clustered_job->getTasks().size());
      // Mote the placeholder job to running
      ongoing_level->pending_placeholder_jobs.erase(placeholder_job);
      ongoing_level->running_placeholder_jobs.insert(placeholder_job);

      // Submit all ready tasks to it each in its standard job
      std::string output_string = "";
      for (auto task : placeholder_job->clustered_job->getTasks()) {
        if (task->getState() == WorkflowTask::READY) {
          StandardJob *standard_job = this->job_manager->createStandardJob(task,{});
          output_string += " " + task->getID();

          WRENCH_INFO("Submitting task %s as part of placeholder job %ld-%ld",
                      task->getID().c_str(), placeholder_job->start_level, placeholder_job->end_level);
          this->job_manager->submitJob(standard_job, placeholder_job->pilot_job->getComputeService());
        } else {
          WRENCH_INFO("Task %s is not ready", task->getID().c_str());
        }
      }

    }


    void LevelByLevelWMS::processEventPilotJobExpiration(std::unique_ptr<PilotJobExpiredEvent> e) {

      // Find the placeholder job
      // Find the placeholder job in the pending list
      PlaceHolderJob *placeholder_job = nullptr;
      OngoingLevel *ongoing_level = nullptr;

      for (auto ol : this->ongoing_levels) {
        for (auto ph : ol.second->running_placeholder_jobs) {
          if (ph->pilot_job == e->pilot_job) {
            placeholder_job = ph;
            ongoing_level = ol.second;
            break;
          }
        }
      }

      if (placeholder_job == nullptr) {
        throw std::runtime_error("Got a pilot job expiration, but no matching placeholder job found");
      }

      WRENCH_INFO("Got a pilot job expiration for a placeholder job that deals with levels %ld-%ld (%s)",
                  placeholder_job->start_level, placeholder_job->end_level, placeholder_job->pilot_job->getName().c_str());


      // Check if there are unprocessed tasks
      bool unprocessed = (placeholder_job->clustered_job->getTasks().size() != placeholder_job->num_completed_tasks);

      if (not unprocessed) { // Nothing to do
        WRENCH_INFO("This placeholder job has no unprocessed tasks. great.");
        return;
      }

      ongoing_level->running_placeholder_jobs.erase(placeholder_job);

      WRENCH_INFO("This placeholder job has unprocessed tasks... resubmit it as a restart");
      // Create a new Clustered Job
      ClusteredJob *cj = new ClusteredJob();
      for (auto t : placeholder_job->clustered_job->getTasks()) {
        if (t->getState() != WorkflowTask::COMPLETED) {
          cj->addTask(t);
        }
      }
      cj->setNumNodes(std::min(placeholder_job->pilot_job->getNumHosts(), cj->getNumTasks()));
      double makespan = cj->estimateMakespan(this->core_speed);

      // Create the pilot job
      PilotJob *pj = this->job_manager->createPilotJob(cj->getNumNodes(), 1, 0, makespan * EXECUTION_TIME_FUDGE_FACTOR);

      PlaceHolderJob *replacement_placeholder_job =
              new PlaceHolderJob(pj, cj,
                                 ongoing_level->level_number, ongoing_level->level_number);

      // Resubmit it!
      ongoing_level->pending_placeholder_jobs.insert(replacement_placeholder_job);
      // submit the corresponding pilot job
      std::map<std::string, std::string> service_specific_args;
      service_specific_args["-N"] = std::to_string(replacement_placeholder_job->pilot_job->getNumHosts());
      service_specific_args["-c"] = std::to_string(replacement_placeholder_job->pilot_job->getNumCoresPerHost());
      service_specific_args["-t"] = std::to_string(1 + ((unsigned long) (replacement_placeholder_job->pilot_job->getDuration())) / 60);
      this->job_manager->submitJob(replacement_placeholder_job->pilot_job, this->batch_service,
                                   service_specific_args);
      WRENCH_INFO("Submitted a Pilot Job (%s hosts, %s min) for workflow level %lu (%s)",
                  service_specific_args["-N"].c_str(),
                  service_specific_args["-t"].c_str(),
                  ongoing_level->level_number,
                  replacement_placeholder_job->pilot_job->getName().c_str());
      WRENCH_INFO("This pilot job has these tasks:");
      for (auto t : replacement_placeholder_job->clustered_job->getTasks()) {
        WRENCH_INFO("     - %s (flops: %lf)", t->getID().c_str(), t->getFlops());
      }

    }


    void LevelByLevelWMS::processEventStandardJobCompletion(std::unique_ptr<StandardJobCompletedEvent> e) {

      WorkflowTask *completed_task = e->standard_job->tasks[0]; // only one task per job

      WRENCH_INFO("Got a standard job completion for task %s", completed_task->getID().c_str());

      // Find the placeholder job this task belongs to
      PlaceHolderJob *placeholder_job = nullptr;
      OngoingLevel *ongoing_level = nullptr;
      for (auto ol : this->ongoing_levels) {
        for (auto ph : ol.second->running_placeholder_jobs) {
          for (auto task : ph->clustered_job->getTasks()) {
            if (task == completed_task) {
              ongoing_level = ol.second;
              placeholder_job = ph;
              break;
            }
          }
        }
      }

      if (placeholder_job == nullptr) {
        throw std::runtime_error("Got a task completion, but couldn't find a placeholder for the task, "
                                         "and we're not in individual mode");
      }

      placeholder_job->num_completed_tasks++;
      // Terminate the pilot job in case all its tasks are done
      if (placeholder_job->num_completed_tasks == placeholder_job->clustered_job->getTasks().size()) {
        WRENCH_INFO("All tasks are completed in this placeholder job, so I am terminating it (%s)",
                    placeholder_job->pilot_job->getName().c_str());
        try {
          this->job_manager->terminateJob(placeholder_job->pilot_job);
        } catch (WorkflowExecutionException &e) {
          // ignore
        }
        ongoing_level->running_placeholder_jobs.erase(placeholder_job);
        ongoing_level->completed_placeholder_jobs.insert(placeholder_job);
      }


      // Start all newly ready tasks that depended on the completed task, IN ANY PLACEHOLDER
      // This shouldn't happen in individual mode, but can't hurt
      WRENCH_INFO("Seeing if other tasks (which are now ready) can be submitted...");
      std::vector<WorkflowTask *>children = this->getWorkflow()->getTaskChildren(completed_task);
      for (auto ol : this->ongoing_levels) {
        for (auto ph : ol.second->running_placeholder_jobs) {

          for (auto task : ph->clustered_job->getTasks()) {
            if ((std::find(children.begin(), children.end(), task) != children.end()) and
                (task->getState() == WorkflowTask::READY)) {
              StandardJob *standard_job = this->job_manager->createStandardJob(task, {});
              WRENCH_INFO("Submitting task %s  as part of placeholder job %ld-%ld",
                          task->getID().c_str(), ph->start_level, ph->end_level);
              this->job_manager->submitJob(standard_job, ph->pilot_job->getComputeService());
            }
          }
        }
      }

      // Remove the ongoing level if it's finished
      if (ongoing_level->pending_placeholder_jobs.empty() and ongoing_level->running_placeholder_jobs.empty()) {
        WRENCH_INFO("Level %ld is finished!", ongoing_level->level_number);
        this->ongoing_levels.erase(ongoing_level->level_number);
      }

    }

    void LevelByLevelWMS::processEventStandardJobFailure(std::unique_ptr<StandardJobFailedEvent> e) {
      WRENCH_INFO("Got a standard job failure event for task %s -- IGNORING THIS (the pilot job expiration event will handle these issues)", e->standard_job->tasks[0]->getID().c_str());
    }

};