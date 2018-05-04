/**
 * Copyright (c) 2017. The WRENCH Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */


#include <wrench-dev.h>
#include "ZhangClusteringWMS.h"
#include "PlaceHolderJob.h"

XBT_LOG_NEW_DEFAULT_CATEGORY(zhang_clustering_wms, "Log category for Zhang Clustering WMS");

namespace wrench {

    ZhangClusteringWMS::ZhangClusteringWMS(std::string hostname, BatchService *batch_service) :
            WMS(nullptr, nullptr, {batch_service}, {}, {}, nullptr, hostname, "clustring_wms") {
      this->batch_service = batch_service;
      this->pending_placeholder_job = nullptr;
    }

    int ZhangClusteringWMS::main() {

      TerminalOutput::setThisProcessLoggingColor(COLOR_WHITE);

      // Find out core speed on the batch service
      this->core_speed = *(this->batch_service->getCoreFlopRate().begin());
      // Find out #hosts on the batch service
      this->num_hosts = this->batch_service->getNumHosts();

      // Create a job manager
      this->job_manager = this->createJobManager();

      while (not workflow->isDone()) {
        // Submit a pilot job (if needed)
        submitPilotJob();

        this->waitForAndProcessNextEvent();

      }

      return 0;
    }



    void ZhangClusteringWMS::submitPilotJob() {

      // Don't schedule a pilot job if one is pending
      if (this->pending_placeholder_job) {
        return;
      }



      // Compute my start level as the next "not started level"
      unsigned long start_level = 0;
      for (auto ph : this->running_placeholder_jobs) {
        start_level = MAX(start_level, ph->end_level);
      }
      start_level = start_level + 1;

      // For now, Just do a single level - TODO: DO THE REAL THING
      unsigned long end_level = start_level;


      // Nothing to do?
      if (start_level >= this->workflow->getNumLevels()) {
        return;
      }

      // Figure out parallelism
      unsigned long parallelism = 0;
      for (unsigned long l = start_level; l <= end_level; l++) {
        unsigned long num_tasks_in_level = this->workflow->getTasksInTopLevelRange(l,l).size();
        if (num_tasks_in_level > this->num_hosts) {
          throw std::runtime_error("ZhangClusteringWMS::submitPilotJob(): Workflow level " +
                                   std::to_string(l) +
                                   " has more tasks than" +
                                   "number of hosts on the batch service, which is not" +
                                   "handled by the algorithm by Zhang et al.");
        }
        parallelism = MAX(parallelism, num_tasks_in_level);
      }

      // Figure out the maximum execution time
      double requested_execution_time = 0;
      for (unsigned long l = start_level; l <= end_level; l++) {
        double max_exec_time_in_level = 0;
        std::vector<WorkflowTask *> tasks_in_level = this->workflow->getTasksInTopLevelRange(l,l);
        for (auto t : tasks_in_level) {
          max_exec_time_in_level = MAX(max_exec_time_in_level,  t->getFlops() / core_speed);
        }
        requested_execution_time += max_exec_time_in_level;
      }

      // Aggregate tasks
      std::vector<WorkflowTask *> tasks;
      for (unsigned long l = start_level; l <= end_level; l++) {
        std::vector<WorkflowTask *> tasks_in_level = this->workflow->getTasksInTopLevelRange(l,l);
        for (auto t : tasks_in_level) {
          if (t->getState() != WorkflowTask::COMPLETED) {
            tasks.push_back(t);
          }
        }
      }

      WRENCH_INFO("Submitting a Pilot Job (%ld hosts, %.2lf sec) for workflow levels %ld-%ld",
                  parallelism, requested_execution_time, start_level, end_level);


      // Submit the pilot job
      std::map<std::string, std::string> service_specific_args;
      service_specific_args["-N"] = std::to_string(parallelism);
      service_specific_args["-c"] = "1";
      service_specific_args["-t"] = std::to_string(1 + ((unsigned long)requested_execution_time)/60);

      this->job_manager->submitJob(this->pending_placeholder_job->pilot_job, this->batch_service, service_specific_args);

      // Keep track of the placeholder job
      this->pending_placeholder_job = new PlaceHolderJob(
              this->job_manager->createPilotJob(parallelism, 1, 0.0, requested_execution_time),
              tasks,
              start_level,
              end_level);

    }


    void ZhangClusteringWMS::processEventPilotJobStart(std::unique_ptr<PilotJobStartedEvent> e) {
      // Just for kicks, check it was the pending one
      if (e->pilot_job != this->pending_placeholder_job->pilot_job) {
        throw std::runtime_error("A pilot job has started, but it doesn't match the pending pilot job!");
      }

      PlaceHolderJob *placeholder_job = this->pending_placeholder_job;

      WRENCH_INFO("The pending pilot job has started!");

      // Move it to running
      this->running_placeholder_jobs.insert(this->pending_placeholder_job);
      this->pending_placeholder_job = nullptr;

      // Submit all ready tasks to it each in its standard job
      for (auto task : placeholder_job->tasks) {
        if (task->getState() == WorkflowTask::READY) {
          StandardJob *standard_job = this->job_manager->createStandardJob(task,{});
          WRENCH_INFO("Submitting a Standard Job to execute Task %s in placeholder %ld-%ld",
                 task->getId().c_str(), placeholder_job->start_level, placeholder_job->end_level);
          this->job_manager->submitJob(standard_job, placeholder_job->pilot_job->getComputeService());
        }
      }

      // Re-submit a pilot job
      this->submitPilotJob();

    }

    void ZhangClusteringWMS::processEventPilotJobExpiration(std::unique_ptr<PilotJobExpiredEvent> e) {

      // Find the placeholder job
      PlaceHolderJob *placeholder_job = nullptr;
      for (auto ph : this->running_placeholder_jobs) {
        if (ph->pilot_job == e->pilot_job) {
          placeholder_job = ph;
          break;
        }
      }
      if (placeholder_job == nullptr) {
        throw std::runtime_error("Got a pilot job expiration, but no matching placeholder job found");
      }

      WRENCH_INFO("Got a pilot job completion for a placeholder job that deals with levels %ld-%ld",
                  placeholder_job->start_level, placeholder_job->end_level);
      // Check if there are unprocessed tasks
      bool unprocessed = false;
      for (auto task : placeholder_job->tasks) {
        if (task->getState() != WorkflowTask::COMPLETED) {
          unprocessed = true;
        }
      }

      if (not unprocessed) { // Nothing to do
        WRENCH_INFO("This placeholder job has no unprocessed tasks. great.");
        return;
      }

      WRENCH_INFO("This placeholder job has unprocessed tasks");

      // Cancel pending pilot job if any
      if (this->pending_placeholder_job) {
        WRENCH_INFO("Canceling pending placeholder job!");
        this->job_manager->terminateJob(this->pending_placeholder_job->pilot_job);
      }

      // Cancel running pilot jobs if none of their tasks has started

      std::set<PlaceHolderJob *> to_remove;
      for (auto ph : this->running_placeholder_jobs) {
        bool started = false;
        for (auto task : ph->tasks) {
          if (task->getState() != WorkflowTask::NOT_READY) {
            started = true;
          }
        }
        if (not started) {
          WRENCH_INFO("Canceling running placeholder job that handled levels %ld-%ld because none"
                              "of its tasks has started", ph->start_level, ph->end_level);
          this->job_manager->terminateJob(ph->pilot_job);
          to_remove.insert(ph);
        }
      }

      for (auto ph : to_remove) {
        this->running_placeholder_jobs.erase(ph);
      }

      // Submit a new pilot job
      submitPilotJob();

    }

    void ZhangClusteringWMS::processEventStandardJobCompletion(std::unique_ptr<StandardJobCompletedEvent> e) {

      WorkflowTask *completed_task = e->standard_job->tasks[0]; // only one task per job

      WRENCH_INFO("Got a standard job completion for task %s", completed_task->getId().c_str());

      // Find the placeholder job this task belongs to, just for kicks
      PlaceHolderJob *placeholder_job = nullptr;
      for (auto ph : this->running_placeholder_jobs) {
        for (auto task : ph->tasks) {
          if (task == completed_task) {
            placeholder_job = ph;
            break;
          }
        }
      }

      if (placeholder_job == nullptr) {
        throw std::runtime_error("Got a task completion, but couldn't find a placeholder for the task");
      }

      // Start all newly ready tasks that depended on the completed task, REGARDLESS OF PLACEHOLDER
      std::vector<WorkflowTask *>children = this->workflow->getTaskChildren(completed_task);
      for (auto ph : this->running_placeholder_jobs) {
        for (auto task : ph->tasks) {
          if ((std::find(children.begin(), children.end(), task) != children.end()) and (task->getState() == WorkflowTask::READY)) {
            StandardJob *standard_job = this->job_manager->createStandardJob(task,{});
            WRENCH_INFO("Submitting a Standard Job to execute Task %s in placeholder %ld-%ld",
                        task->getId().c_str(), ph->start_level, ph->end_level);
            this->job_manager->submitJob(standard_job, ph->pilot_job->getComputeService());
          }
        }
      }
    }

    void ZhangClusteringWMS::processEventStandardJobFailure(std::unique_ptr<StandardJobFailedEvent> e) {
      WRENCH_INFO("Got a standard job failure event for task %s -- IGNORING THIS",
              e->standard_job->tasks[0]->getId().c_str());

    }

};