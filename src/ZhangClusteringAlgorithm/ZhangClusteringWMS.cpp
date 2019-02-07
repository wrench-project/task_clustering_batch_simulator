/**
 * Copyright (c) 2017. The WRENCH Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */


#include <wrench-dev.h>
#include <Util/WorkflowUtil.h>
#include <Simulator.h>
#include "ZhangClusteringWMS.h"
#include "ZhangPlaceHolderJob.h"

XBT_LOG_NEW_DEFAULT_CATEGORY(zhang_clustering_wms,
"Log category for Zhang Clustering WMS");

#define EXECUTION_TIME_FUDGE_FACTOR 1.1

namespace wrench {

    class Simulator;

    static double parent_runtime = 0;
    static int sequence = 0;

    ZhangClusteringWMS::ZhangClusteringWMS(Simulator *simulator, std::string hostname, bool overlap, bool plimit,
                                           BatchService *batch_service) :
            WMS(nullptr, nullptr, {batch_service}, {}, {}, nullptr, hostname, "clustering_wms") {
        this->simulator = simulator;
        this->overlap = overlap;
        this->plimit = plimit;
        this->batch_service = batch_service;
        this->pending_placeholder_job = nullptr;
        this->individual_mode = false;
    }

    int ZhangClusteringWMS::main() {

        TerminalOutput::setThisProcessLoggingColor(TerminalOutput::COLOR_WHITE);

        this->checkDeferredStart();

        // Find out core speed on the batch service
        this->core_speed = (*(this->batch_service->getCoreFlopRate().begin())).second;
        // Find out #hosts on the batch service
        this->number_of_hosts = this->batch_service->getNumHosts();

        // Create a job manager
        this->job_manager = this->createJobManager();

        while (not this->getWorkflow()->isDone()) {

            // Submit a pilot job (if needed)
            applyGroupingHeuristic();

            this->waitForAndProcessNextEvent();

        }
        return 0;
    }

    void ZhangClusteringWMS::applyGroupingHeuristic() {

//      WRENCH_INFO("APPLYING GROUPING HEURISTIC");

        // Don't schedule a pilot job if one is pending
        if (this->pending_placeholder_job) {
            return;
        }

        // Don't schedule a pilot job if we're in individual mode
        if (this->individual_mode) {
            return;
        }

        // Don't schedule a pilot job is overlap = false and anything is running
        if ((not this->overlap) and (not this->running_placeholder_jobs.empty())) {
            return;
        }

        unsigned long start_level = getStartLevel();
        unsigned long end_level = this->getWorkflow()->getNumLevels() - 1;

        // Nothing to do?
        if (start_level > end_level) {
            return;
        }

        // peelLevel() (Fig. 4 in the paper)
        double runtime_all, wait_time_all;
        double peel_runtime[2], peel_wait_time[2];

        unsigned long max_parallelism = maxParallelism(start_level, end_level);

        // calculate the runtime of entire DAG
        runtime_all = WorkflowUtil::estimateMakespan(
                this->getWorkflow()->getTasksInTopLevelRange(start_level, end_level),
                max_parallelism, this->core_speed);
        wait_time_all = estimateWaitTime(max_parallelism, runtime_all, &sequence);

        peel_runtime[0] = runtime_all;
        peel_wait_time[0] = wait_time_all;

        // See if we can do better by grouping (Fig. 5 in the paper)
        // return params: wait_time, makespan, end_level
        std::tuple<double, double, unsigned long> partial_dag = groupLevels(start_level, end_level, peel_runtime,
                                                                            peel_wait_time);
        double partial_dag_wait_time = std::get<0>(partial_dag);
        double partial_dag_makespan = std::get<1>(partial_dag);
        unsigned long partial_dag_end_level = std::get<2>(partial_dag);

        if (partial_dag_end_level >= end_level) {
            if (runtime_all * 2.0 < wait_time_all) {
                // submit remaining dag as 1 job
            } else {
                // submit remaining dag as 1 job per task
            }
            this->individual_mode = true;
        }

        if (this->individual_mode) {
            WRENCH_INFO("GROUPING: INDIVIDUAL");
        } else {
            WRENCH_INFO("GROUPING: %ld-%ld",
                        start_level, end_level);
        }

        if (not individual_mode) {
            // recalculate parallelism for partial dag
            unsigned long parallelism = maxParallelism(start_level, partial_dag_end_level);
            createAndSubmitPlaceholderJob(
                    partial_dag_makespan,
                    parallelism,
                    start_level,
                    partial_dag_end_level);
        } else {
            WRENCH_INFO("Switching to individual mode!");
            // Submit all READY tasks as individual jobs
            for (auto task : this->getWorkflow()->getTasks()) {
                if (task->getState() == WorkflowTask::State::READY) {
                    StandardJob *standard_job = this->job_manager->createStandardJob(task, {});
                    std::map <std::string, std::string> service_specific_args;
                    unsigned long requested_execution_time =
                            (task->getFlops() / this->core_speed) * EXECUTION_TIME_FUDGE_FACTOR;
                    service_specific_args["-N"] = "1";
                    service_specific_args["-c"] = "1";
                    service_specific_args["-t"] = std::to_string(1 + ((unsigned long) requested_execution_time) / 60);
                    WRENCH_INFO("Submitting task %s individually!", task->getID().c_str());
                    this->job_manager->submitJob(standard_job, this->batch_service, service_specific_args);
                }
            }
        }

    }

    /**
     *
     * @param requested_execution_time
     * @param requested_parallelism
     * @param start_level
     * @param end_level
     */
    void ZhangClusteringWMS::createAndSubmitPlaceholderJob(
            double requested_execution_time,
            unsigned long requested_parallelism,
            unsigned long start_level,
            unsigned long end_level) {

        requested_execution_time = requested_execution_time * EXECUTION_TIME_FUDGE_FACTOR;

        // Set global parent runtime to use for leeway calculation
        parent_runtime = requested_execution_time;

        // Aggregate tasks
        std::vector < WorkflowTask * > tasks;
        for (unsigned long l = start_level; l <= end_level; l++) {
            std::vector < WorkflowTask * > tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(l, l);
            for (auto t : tasks_in_level) {
                if (t->getState() != WorkflowTask::COMPLETED) {
                    tasks.push_back(t);
                }
            }
        }

        // Submit the pilot job
        std::map <std::string, std::string> service_specific_args;
        service_specific_args["-N"] = std::to_string(requested_parallelism);
        service_specific_args["-c"] = "1";
        service_specific_args["-t"] = std::to_string(1 + ((unsigned long) requested_execution_time) / 60);


        // Keep track of the placeholder job
        this->pending_placeholder_job = new ZhangPlaceHolderJob(
                this->job_manager->createPilotJob(),
                tasks,
                start_level,
                end_level);

        WRENCH_INFO("Submitting a Pilot Job (%ld hosts, %.2lf sec) for workflow levels %ld-%ld (%s)",
                    requested_parallelism, requested_execution_time, start_level, end_level,
                    this->pending_placeholder_job->pilot_job->getName().c_str());
        WRENCH_INFO("This pilot job has these tasks:");
        for (auto t : this->pending_placeholder_job->tasks) {
            WRENCH_INFO("     - %s", t->getID().c_str());
        }

        // submit the corresponding pilot job
        this->job_manager->submitJob(this->pending_placeholder_job->pilot_job, this->batch_service,
                                     service_specific_args);
    }


    void ZhangClusteringWMS::processEventPilotJobStart(std::unique_ptr <PilotJobStartedEvent> e) {

        // Update queue waiting time
        this->simulator->total_queue_wait_time +=
                this->simulation->getCurrentSimulatedDate() - e->pilot_job->getSubmitDate();

        // Just for kicks, check it was the pending one
        WRENCH_INFO("Got a Pilot Job Start event: %s", e->pilot_job->getName().c_str());
        if (this->pending_placeholder_job == nullptr) {
            throw std::runtime_error("Fatal Error: couldn't find a placeholder job for a pilob job that just started");
        }
//      WRENCH_INFO("Got a Pilot Job Start event e->pilot_job = %ld, this->pending->pilot_job = %ld (%s)",
//                  (unsigned long) e->pilot_job,
//                  (unsigned long) this->pending_placeholder_job->pilot_job,
//                  this->pending_placeholder_job->pilot_job->getName().c_str());

        if (e->pilot_job != this->pending_placeholder_job->pilot_job) {

            WRENCH_INFO("Must be for a placeholder I already cancelled... nevermind");
            return;
        }

        ZhangPlaceHolderJob *placeholder_job = this->pending_placeholder_job;

        // Move it to running
        this->running_placeholder_jobs.insert(placeholder_job);
        this->pending_placeholder_job = nullptr;

        // Submit all ready tasks to it each in its standard job
        std::string output_string = "";
        for (auto task : placeholder_job->tasks) {
            if (task->getState() == WorkflowTask::READY) {
                StandardJob *standard_job = this->job_manager->createStandardJob(task, {});
                output_string += " " + task->getID();

                WRENCH_INFO("Submitting task %s as part of placeholder job %ld-%ld",
                            task->getID().c_str(), placeholder_job->start_level, placeholder_job->end_level);
                this->job_manager->submitJob(standard_job, placeholder_job->pilot_job->getComputeService());
            }
        }

        // Re-submit a pilot job so as to overlap execution of job n with waiting of job n+1
        this->applyGroupingHeuristic();

    }

    void ZhangClusteringWMS::processEventPilotJobExpiration(std::unique_ptr <PilotJobExpiredEvent> e) {
        std::cout << "JOB EXPIRATION!!!" << std::endl;

        // Find the placeholder job
        ZhangPlaceHolderJob *placeholder_job = nullptr;
        for (auto ph : this->running_placeholder_jobs) {
            if (ph->pilot_job == e->pilot_job) {
                placeholder_job = ph;
                break;
            }
        }
        if (placeholder_job == nullptr) {
            throw std::runtime_error("Got a pilot job expiration, but no matching placeholder job found");
        }

        // Remove it from the list of running pilot jobs
        this->running_placeholder_jobs.erase(placeholder_job);

        WRENCH_INFO("Got a pilot job expiration for a placeholder job that deals with levels %ld-%ld (%s)",
                    placeholder_job->start_level, placeholder_job->end_level,
                    placeholder_job->pilot_job->getName().c_str());
        // Check if there are unprocessed tasks
        bool unprocessed = false;
        for (auto t : placeholder_job->tasks) {
            if (t->getState() != WorkflowTask::COMPLETED) {
                unprocessed = true;
                break;
            }
        }

//      double wasted_node_seconds = e->pilot_job->getNumHosts() * e->pilot_job->getDuration();
        unsigned long num_used_nodes;
        sscanf(e->pilot_job->getServiceSpecificArguments()["-N"].c_str(), "%lu", &num_used_nodes);
        unsigned long num_used_minutes;
        sscanf(e->pilot_job->getServiceSpecificArguments()["-t"].c_str(), "%lu", &num_used_minutes);
        double wasted_node_seconds = 60.0 * num_used_minutes * num_used_nodes;
        for (auto t : placeholder_job->tasks) {
            if (t->getState() == WorkflowTask::COMPLETED) {
                wasted_node_seconds -= t->getFlops() / this->core_speed;
            }
        }
        this->simulator->wasted_node_seconds += wasted_node_seconds;

        if (not unprocessed) { // Nothing to do
            WRENCH_INFO("This placeholder job has no unprocessed tasks. great.");
            return;
        }

        this->simulator->num_pilot_job_expirations_with_remaining_tasks_to_do++;

        WRENCH_INFO("This placeholder job has unprocessed tasks");

        // Cancel pending pilot job if any
        if (this->pending_placeholder_job) {
            WRENCH_INFO("Canceling pending placeholder job (placeholder=%ld,  pilot_job=%ld / %s",
                        (unsigned long) this->pending_placeholder_job,
                        (unsigned long) this->pending_placeholder_job->pilot_job,
                        this->pending_placeholder_job->pilot_job->getName().c_str());
            this->job_manager->terminateJob(this->pending_placeholder_job->pilot_job);
            this->pending_placeholder_job = nullptr;
        }

        // Cancel running pilot jobs if none of their tasks has started

        std::set < ZhangPlaceHolderJob * > to_remove;
        for (auto ph : this->running_placeholder_jobs) {
            bool started = false;
            for (auto task : ph->tasks) {
                if (task->getState() != WorkflowTask::NOT_READY) {
                    started = true;
                }
            }
            if (not started) {
                WRENCH_INFO("Canceling running placeholder job that handled levels %ld-%ld because none"
                            "of its tasks has started (%s)", ph->start_level, ph->end_level,
                            ph->pilot_job->getName().c_str());
                try {
                    this->job_manager->terminateJob(ph->pilot_job);
                } catch (WorkflowExecutionException &e) {
                    // ignore (likely already dead!)
                }
                to_remove.insert(ph);
            }
        }

        for (auto ph : to_remove) {
            this->running_placeholder_jobs.erase(ph);
        }

        // Make decisions again
        applyGroupingHeuristic();

    }

    void ZhangClusteringWMS::processEventStandardJobCompletion(std::unique_ptr <StandardJobCompletedEvent> e) {

        WorkflowTask *completed_task = e->standard_job->tasks[0]; // only one task per job

        WRENCH_INFO("Got a standard job completion for task %s", completed_task->getID().c_str());

        this->simulator->used_node_seconds += completed_task->getFlops() / this->core_speed;

        // Find the placeholder job this task belongs to
        ZhangPlaceHolderJob *placeholder_job = nullptr;
        for (auto ph : this->running_placeholder_jobs) {
            for (auto task : ph->tasks) {
                if (task == completed_task) {
                    placeholder_job = ph;
                    break;
                }
            }
        }

        if ((placeholder_job == nullptr) and (not this->individual_mode)) {
            throw std::runtime_error("Got a task completion, but couldn't find a placeholder for the task, "
                                     "and we're not in individual mode");
        }

        if (placeholder_job != nullptr) {

            // Terminate the pilot job in case all its tasks are done
            bool all_tasks_done = true;
            for (auto t : placeholder_job->tasks) {
                if (t->getState() != WorkflowTask::COMPLETED) {
                    all_tasks_done = false;
                    break;
                }
            }
            if (all_tasks_done) {
                WRENCH_INFO("All tasks are completed in this placeholder job, so I am terminating it (%s)",
                            placeholder_job->pilot_job->getName().c_str());
                try {
                    WRENCH_INFO("TERMINATING A PILOT JOB");
                    this->job_manager->terminateJob(placeholder_job->pilot_job);
                } catch (WorkflowExecutionException &e) {
                    // ignore
                }
                this->running_placeholder_jobs.erase(placeholder_job);
            }


        }

        // Start all newly ready tasks that depended on the completed task, IN ANY PLACEHOLDER
        // This shouldn't happen in individual mode, but can't hurt
        std::vector < WorkflowTask * > children = this->getWorkflow()->getTaskChildren(completed_task);
        for (auto ph : this->running_placeholder_jobs) {
            for (auto task : ph->tasks) {
                if ((std::find(children.begin(), children.end(), task) != children.end()) and
                    (task->getState() == WorkflowTask::READY)) {
                    StandardJob *standard_job = this->job_manager->createStandardJob(task, {});
                    WRENCH_INFO("Submitting task %s  as part of placeholder job %ld-%ld",
                                task->getID().c_str(), placeholder_job->start_level, placeholder_job->end_level);
                    this->job_manager->submitJob(standard_job, ph->pilot_job->getComputeService());
                }
            }
        }

        if (this->individual_mode) {
            for (auto task : this->getWorkflow()->getTasks()) {
                if (task->getState() == WorkflowTask::State::READY) {
                    StandardJob *standard_job = this->job_manager->createStandardJob(task, {});
                    WRENCH_INFO("Submitting task %s individually!",
                                task->getID().c_str());
                    std::map <std::string, std::string> service_specific_args;
                    double requested_execution_time =
                            (task->getFlops() / this->core_speed) * EXECUTION_TIME_FUDGE_FACTOR;
                    service_specific_args["-N"] = "1";
                    service_specific_args["-c"] = "1";
                    service_specific_args["-t"] = std::to_string(1 + ((unsigned long) requested_execution_time) / 60);
                    this->job_manager->submitJob(standard_job, this->batch_service, service_specific_args);
                }
            }
        }


    }

    void ZhangClusteringWMS::processEventStandardJobFailure(std::unique_ptr <StandardJobFailedEvent> e) {
        WRENCH_INFO("Got a standard job failure event for task %s -- IGNORING THIS",
                    e->standard_job->tasks[0]->getID().c_str());
    }


    /**
     *
     * @param start_level
     * @param end_level
     * @return
     */
    std::tuple<double, double, unsigned long> ZhangClusteringWMS::computeLevelGroupingRatio(
            unsigned long start_level, unsigned long end_level) {

//        static int sequence = 0;

        // Figure out parallelism
        unsigned long parallelism = 0;
        for (unsigned long l = start_level; l <= end_level; l++) {
            unsigned long num_tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(l, l).size();
            if (this->plimit) {
                if (num_tasks_in_level > this->number_of_hosts) {
                    throw std::runtime_error("ZhangClusteringWMS::applyGroupingHeuristic(): Workflow level " +
                                             std::to_string(l) +
                                             " has more tasks than " +
                                             "number of hosts on the batch service, which is not " +
                                             "handled by the algorithm by Zhang et al.");
                }
            }
            unsigned long level_parallelism = std::min<unsigned long>(num_tasks_in_level, this->number_of_hosts);
            parallelism = std::max<unsigned long>(parallelism, level_parallelism);
        }

        // At this point, parallelism is the max parallelism in the DAG

//      WRENCH_INFO("THERE ARE %ld tasks in level range %ld-%ld",
//              this->getWorkflow()->getTasksInTopLevelRange(start_level, end_level).size(), start_level, end_level);

        unsigned long picked_parallelism = ULONG_MAX;
        double best_makespan = -1.0;

        if (this->plimit) { //Ensure strict application of Zhang's
            picked_parallelism = parallelism;
            best_makespan = WorkflowUtil::estimateMakespan(
                    this->getWorkflow()->getTasksInTopLevelRange(start_level, end_level),
                    picked_parallelism, this->core_speed);

        } else { // Fix Zhang Problem #1 and also potentially improves resource usage for smaller jobs

            // Figure out the maximum execution time
            for (unsigned long i = 1; i <= parallelism; i++) {

                double makespan = WorkflowUtil::estimateMakespan(
                        this->getWorkflow()->getTasksInTopLevelRange(start_level, end_level),
                        i, this->core_speed);
                if ((best_makespan < 0) or (makespan < best_makespan)) {
                    picked_parallelism = i;
                    best_makespan = makespan;
                }
            }
        }

        // Here we have decided the number of hosts and the makespan
        // We now ask: what's the wait time?
        //  (so that we can inform the caller)

        // Figure out the estimated wait time
//      std::set<std::tuple<std::string,unsigned int,unsigned int, double>> job_config;
//      std::string config_key = "config_XXXX_" + std::to_string(sequence++); // need to make it unique for BATSCHED
//      job_config.insert(std::make_tuple(config_key, (unsigned int)picked_parallelism, 1, best_makespan));
//      std::map<std::string, double> estimates = this->batch_service->getStartTimeEstimates(job_config);
//      if (estimates[config_key] < 0) {
//        throw std::runtime_error("Could not obtain start time estimate... aborting");
//      }
//      double wait_time_estimate = std::max<double>(0, estimates[config_key] - this->simulation->getCurrentSimulatedDate());
        double wait_time_estimate = 0;
        double leeway = 0;
        /**
        do {
            best_makespan = best_makespan + leeway / 2;
            wait_time_estimate = estimateWaitTime(picked_parallelism, best_makespan, &sequence);
            leeway = parent_runtime + best_makespan - wait_time_estimate;
            std::cout << "LEEWAY: " << leeway << std::endl;
            std::cout << "WAIT: " << wait_time_estimate << std::endl;
            std::cout << (leeway > 600) << std::endl;
        } while (leeway > 600);
        **/

        wait_time_estimate = estimateWaitTime(picked_parallelism, best_makespan, &sequence);
        leeway = parent_runtime - wait_time_estimate;
        best_makespan = (leeway > 0) ? (best_makespan + leeway) : best_makespan;
        // this would be an unnecessary call if leeway was < 0
        wait_time_estimate = estimateWaitTime(picked_parallelism, best_makespan, &sequence);

//      double real_wait_time = (wait_time_estimate - parent_runtime < 0) ? wait_time_estimate : (wait_time_estimate - parent_runtime);

        WRENCH_INFO("GroupLevel(%ld,%ld): parallelism=%ld, wait_time=%.2lf, execution_time=%.2lf",
                    start_level, end_level, picked_parallelism, wait_time_estimate, best_makespan);

        return std::make_tuple(wait_time_estimate, best_makespan, picked_parallelism);
    }

    double ZhangClusteringWMS::estimateWaitTime(long parallelism, double makespan, int *sequence) {
        std::set <std::tuple<std::string, unsigned int, unsigned int, double>> job_config;
        std::string config_key = "config_XXXX_" + std::to_string((*sequence)++); // need to make it unique for BATSCHED
        job_config.insert(std::make_tuple(config_key, (unsigned int) parallelism, 1, makespan));
        std::map<std::string, double> estimates = this->batch_service->getStartTimeEstimates(job_config);

        if (estimates[config_key] < 0) {
            throw std::runtime_error("Could not obtain start time estimate... aborting");
        }

        double wait_time_estimate = std::max<double>(0, estimates[config_key] -
                                                        this->simulation->getCurrentSimulatedDate());
        return wait_time_estimate;
    }

    // Compute my start level first as the first level that's not fully completed
    unsigned long ZhangClusteringWMS::getStartLevel() {
        unsigned long start_level = 0;
        for (unsigned long i = 0; i < this->getWorkflow()->getNumLevels(); i++) {
            std::vector < WorkflowTask * > tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(i, i);
            bool all_completed = true;
            for (auto task : tasks_in_level) {
                if (task->getState() != WorkflowTask::State::COMPLETED) {
                    all_completed = false;
                }
            }
            if (all_completed) {
                start_level = i + 1;
            }
        }

        for (auto ph : this->running_placeholder_jobs) {
            start_level = 1 + std::max<unsigned long>(start_level, ph->end_level);
        }

        return start_level;
    }

    // Zhang is supposed to fail automatically if number of tasks > number of hosts
    // Just return max hosts to avoid failure for now
    // end_level = num_levels - 1
    unsigned long ZhangClusteringWMS::maxParallelism(unsigned long start_level, unsigned long end_level) {
        unsigned long parallelism = 0;
        for (unsigned long i = start_level; i <= end_level; i++) {
            unsigned long num_tasks_in_level = this->getWorkflow()->getTasksInTopLevelRange(i, i).size();
            parallelism = std::max<unsigned long>(parallelism, num_tasks_in_level);
        }

        return std::min<unsigned long>(parallelism, this->number_of_hosts);
    }

    // return params: wait_time, run_time, end_level
    std::tuple<double, double, unsigned long>
    ZhangClusteringWMS::groupLevels(unsigned long start_level, unsigned long end_level, double peel_runtime[2],
                                    double peel_wait_time[2]) {
        double real_runtime[2];
        double runtime_all, wait_time_all, leeway;
        runtime_all = peel_runtime[0];
        wait_time_all = peel_wait_time[0];
        real_runtime[0] = peel_runtime[0];
        bool giant = true;
        // Start partial dag with first level
        unsigned long candidate_end_level = start_level;
        while (candidate_end_level < end_level) {
            std::cout << "START LEVEL: " << start_level << std::endl;
            std::cout << "END LEVEL: " << candidate_end_level << std::endl;
            unsigned long max_parallelism = maxParallelism(start_level, candidate_end_level);
            double partial_dag_runtime = WorkflowUtil::estimateMakespan(
                    this->getWorkflow()->getTasksInTopLevelRange(start_level, candidate_end_level),
                    max_parallelism, this->core_speed);
            peel_runtime[1] = partial_dag_runtime;
            real_runtime[1] = peel_runtime[1];
            // Modifying original algo. from here
            peel_wait_time[1] = estimateWaitTime(max_parallelism, peel_runtime[1], &sequence);
            std::cout << "RUNTIME: " << peel_runtime[1] << std::endl;
            std::cout << "WAITTIME: " << peel_wait_time[1] << std::endl;
            std::cout << "PARENT RUNTIME: " << parent_runtime << std::endl;
            double overlap = (peel_runtime[1] + peel_wait_time[1] + leeway) - parent_runtime;
            /**
            while (overlap < 0) {
                peel_runtime[1] = peel_runtime[1] + leeway / 2;
                peel_wait_time[1] = estimateWaitTime(max_parallelism, peel_runtime[1], &sequence);
                leeway = parent_runtime + real_runtime[1] - peel_wait_time[1];
                std::cout << "PEEL RUNTIME: " << peel_runtime[1] << std::endl;
                std::cout << "PEEL WAIT TIME: " << peel_wait_time[1] << std::endl;
                std::cout << "LEEWAY: " << leeway << std::endl;
                std::cout << "leeway > 600 -> " << (leeway > 600) << std::endl;
                // 3 issues:
                // if parent_runtime is 0 i.e. fist task running
                // if wait time stops increasing due to no other jobs in line i.e leeway->infinity, but wait is constant
                // just add enough leeway to overlap parent run with current wait and runtime
                overlap = (peel_runtime[1] + peel_wait_time[1] + leeway) - parent_runtime;
                // TODO - recalculate wait time if doing like this
            }
            */
            if (parent_runtime <= 0) {
                // no leeway needed
                std::cout << "PARENT RUNTIME <= 0" << std::endl;
            } else if (parent_runtime > peel_wait_time[1]) {
                leeway = parent_runtime - peel_wait_time[1];
                std::cout << "LEEWAY: " << leeway << std::endl;
            } else {
                // no leeway needed
            }

            // Resuming original algo. here
            if (leeway > 0) {
                peel_runtime[1] = peel_runtime[1] + leeway;
                // recalculate wait if runtime was modified
                // this may lead to some unneccessary leeway if wait increases significantly
                peel_wait_time[1] = estimateWaitTime(max_parallelism, peel_runtime[1], &sequence);
            }
            double real_wait_time = peel_wait_time[1] - parent_runtime;
            if (real_wait_time < 0) {
                real_wait_time = peel_wait_time[1];
            }
            if (giant) {
                if (real_wait_time > real_runtime[1]) {
                    candidate_end_level++;
                    continue;
                }
            }
            giant = false;
            if (peel_wait_time[1] - parent_runtime > 0) {
                if (peel_wait_time[1] / real_runtime[1] > peel_wait_time[0] / real_runtime[0]) {
                    break;
                } else if (peel_wait_time[1] / real_runtime[1] > wait_time_all / runtime_all) {
                    break;
                }
            }
            peel_wait_time[0] = peel_wait_time[1];
            peel_runtime[0] = peel_runtime[1];
            real_runtime[0] = real_runtime[1];
            candidate_end_level++;
        }
        if (giant) {
            std::cout << "RETURNING GIANT" << std::endl;
            // return whole thing
            // going to run static algo if partial dag = DAG, so makespan and wait don't matter
            return std::make_tuple(0, 0, end_level);
        } else {
            std::cout << "SPLITTING, END LEVEL=" << candidate_end_level << std::endl;
            // return partial dag
            return std::make_tuple(peel_wait_time[1], peel_runtime[1], candidate_end_level);
        }
    }

};
