///////////////////////////////////////////////////////////////////////////////
// BSD 3-Clause License
//
// Copyright (C) 2018-2019, Max Planck Gesellschaft
// Copyright note valid unless otherwise stated in individual files.
// All rights reserved.
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <pybind11/embed.h>

#include <real_time_tools/checkpoint_timer.hpp>
#include <real_time_tools/process_manager.hpp>
#include <real_time_tools/thread.hpp>

#include <signal_handler/signal_handler.hpp>

#include <robot_interfaces/loggable.hpp>
#include <robot_interfaces/robot_data.hpp>
#include <robot_interfaces/robot_driver.hpp>
#include <robot_interfaces/status.hpp>

namespace robot_interfaces
{
/**
 * @brief Communication link between RobotDriver and RobotData.
 *
 * At each time-step, it gets the observation from the RobotDriver and
 * writes it to RobotData, and it takes the desired_action from RobotData
 * and applies it on the RobotDriver.
 *
 * @tparam Action
 * @tparam Observation
 */
template <typename Action, typename Observation>
class RobotBackend
{
public:
    /**
     * @param robot_driver  Driver instance for the actual robot.
     * @param robot_data  Data is send to/retrieved from here.
     * @param real_time_mode  Enable/disable real-time mode.  In real-time mode,
     *     the backend will repeat previous actions if the new one is not
     *     provided in time or fail with an error if the allowed number of
     *     repetitions is exceeded.  In non-real-time mode, it will simply block
     *     and wait until the action is provided.
     * @param first_action_timeout  See RobotBackend::first_action_timeout_.
     * @param max_number_of_actions  See RobotBackend::max_number_of_actions_.
     */
    RobotBackend(std::shared_ptr<RobotDriver<Action, Observation>> robot_driver,
                 std::shared_ptr<RobotData<Action, Observation>> robot_data,
                 const bool real_time_mode = true,
                 const double first_action_timeout =
                     std::numeric_limits<double>::infinity(),
                 const uint32_t max_number_of_actions = 0)
        : robot_driver_(robot_driver),
          robot_data_(robot_data),
          real_time_mode_(real_time_mode),
          first_action_timeout_(first_action_timeout),
          max_number_of_actions_(max_number_of_actions),
          is_shutdown_requested_(false),
          max_action_repetitions_(0)
    {
        signal_handler::SignalHandler::initialize();

        loop_is_running_ = true;
        thread_ = std::make_shared<real_time_tools::RealTimeThread>();
        thread_->create_realtime_thread(&RobotBackend::loop, this);
    }

    virtual ~RobotBackend()
    {
        // pybind11::gil_scoped_release causes a segfault when the class is used
        // directly from C++ (i.e. no Python interpreter running).
        // Best workaround found so far is to explicitly check if Python is
        // initialized or not...
        // See https://github.com/pybind/pybind11/issues/2177
        if (Py_IsInitialized())
        {
            // Release the GIL when destructing the backend, as otherwise the
            // program will get stuck in a dead lock in case the driver needs to
            // run some Python code.
            pybind11::gil_scoped_release release;

            request_shutdown();
            thread_->join();
        }
        else
        {
            request_shutdown();
            thread_->join();
        }
    }

    uint32_t get_max_action_repetitions()
    {
        return max_action_repetitions_;
    }

    /**
     * @brief Set how often an action is repeated if no new one is provided.
     *
     * If the next action is due to be executed but the user did not provide
     * one yet (i.e. there is no new action in the robot data time series),
     * the last action will be repeated by automatically adding it to the
     * time series again.
     *
     * Use this this method to specify how often the action shall be
     * repeated (default is 0, i.e. no repetition at all).  If this limit is
     * exceeded, the robot will be shut down and the RobotBackend stops.
     *
     * **Note:** This is ignored in non-real-time mode.
     *
     * @param max_action_repetitions
     */
    void set_max_action_repetitions(const uint32_t &max_action_repetitions)
    {
        max_action_repetitions_ = max_action_repetitions;
    }

    void initialize()
    {
        robot_driver_->initialize();
    }

    /**
     * @brief Request shutdown of the backend loop.
     *
     * The loop may take some time to actually terminate after calling this
     * function. Use wait_until_terminated() to ensure it has really terminated.
     */
    void request_shutdown()
    {
        is_shutdown_requested_ = true;
    }

    /**
     * @brief Wait until the backend loop terminates.
     */
    void wait_until_terminated() const
    {
        while (loop_is_running_)
        {
            real_time_tools::Timer::sleep_microseconds(100000);
        }
    }

private:
    std::shared_ptr<RobotDriver<Action, Observation>> robot_driver_;
    std::shared_ptr<RobotData<Action, Observation>> robot_data_;

    /**
     * @brief Enable/disable real time mode.
     *
     * If real time mode is enabled (true), the back end expects new actions to
     * be provided in time by the user.  If this does not happen, the last
     * received action is repeated until the configured number of repetitions is
     * exceeded in which case it stops with an error.
     *
     * If real time mode is disabled (false), the back-end loop blocks and waits
     * for the next action if it is not provided in time.
     *
     * @see max_action_repetitions_
     */
    const bool real_time_mode_;

    /**
     * @brief Timeout for the first action to arrive.
     *
     * Timeout for the time between starting the backend loop and receiving the
     * first action from the user.  If exceeded, the backend shuts down.
     * Set to infinity to disable the timeout.
     */
    const double first_action_timeout_;

    /**
     * @brief Maximum number of actions that are executed by the backend.
     *
     * If set to a value greater than zero, the backend will automatically shut
     * down after the specified number of actions is executed.
     */
    const uint32_t max_number_of_actions_;

    /**
     * @brief Set to true when shutdown is requested.
     *
     * This is used to notify the background loop about requested shutdown, so
     * it terminates itself.
     */
    std::atomic<bool> is_shutdown_requested_;

    //! @brief Indicates if the background loop is still running.
    std::atomic<bool> loop_is_running_;

    /**
     * @brief Number of times the previous action is repeated if no new one
     *        is provided.
     */
    uint32_t max_action_repetitions_;

    real_time_tools::CheckpointTimer<6, false> timer_;

    std::shared_ptr<real_time_tools::RealTimeThread> thread_;

    bool has_shutdown_request() const
    {
        return is_shutdown_requested_ ||
               signal_handler::SignalHandler::has_received_sigint();
    }

    // control loop
    // ------------------------------------------------------------
    static void *loop(void *instance_pointer)
    {
        ((RobotBackend *)(instance_pointer))->loop();
        return nullptr;
    }

    /**
     * @brief Main loop.
     *
     * Iterate over robot_data_.desired_action and apply these actions to
     * the robot, and read the applied_action and the observation from the
     * robot and append them to the corresponding timeseries in robot_data_.
     */
    void loop()
    {
        const double start_time =
            real_time_tools::Timer::get_current_time_sec();

        // wait until first desired_action was received
        // ----------------------------
        while (!has_shutdown_request() &&
               !robot_data_->desired_action->wait_for_timeindex(0, 0.1))
        {
            const double now = real_time_tools::Timer::get_current_time_sec();
            if (now - start_time > first_action_timeout_)
            {
                Status status;
                status.set_error(Status::ErrorStatus::BACKEND_ERROR,
                                 "First action was not provided in time");

                robot_data_->status->append(status);

                std::cerr << "Error: " << status.error_message
                          << "\nRobot is shut down." << std::endl;

                request_shutdown();
                break;
            }
        }

        for (long int t = 0; !has_shutdown_request(); t++)
        {
            // TODO: figure out latency stuff!!

            Status status;

            if (max_number_of_actions_ > 0 && t >= max_number_of_actions_)
            {
                // TODO this is not really an error
                status.set_error(Status::ErrorStatus::BACKEND_ERROR,
                                 "Maximum number of actions reached.");
            }

            timer_.start();

            // get latest observation from robot and append it to robot_data_
            Observation observation = robot_driver_->get_latest_observation();
            timer_.checkpoint("get observation");

            robot_data_->observation->append(observation);
            // TODO: for some reason this sometimes takes more than 2 ms
            // i think this may be due to a non-realtime thread blocking the
            // timeseries. this is in fact an issue, we might have to
            // duplicate all the timeseries and have a realtime thread
            // writing back and forth
            timer_.checkpoint("append observation");

            // If real time mode is enabled the next action needs to be provided
            // in time.  If this is not the case, optionally repeat the previous
            // action or raise an error.
            if (real_time_mode_ &&
                robot_data_->desired_action->newest_timeindex() < t)
            {
                uint32_t action_repetitions =
                    robot_data_->status->newest_element().action_repetitions;

                if (action_repetitions < max_action_repetitions_)
                {
                    robot_data_->desired_action->append(
                        robot_data_->desired_action->newest_element());
                    status.action_repetitions = action_repetitions + 1;
                }
                else
                {
                    // No action provided and number of allowed repetitions
                    // of the previous action is exceeded --> Error
                    status.set_error(Status::ErrorStatus::BACKEND_ERROR,
                                     "Next action was not provided in time");
                }
            }

            std::string driver_error_msg = robot_driver_->get_error();
            if (!driver_error_msg.empty())
            {
                status.set_error(Status::ErrorStatus::DRIVER_ERROR,
                                 driver_error_msg);
            }

            robot_data_->status->append(status);

            // if there is an error, shut robot down and stop loop
            if (status.error_status != Status::ErrorStatus::NO_ERROR)
            {
                std::cerr << "Error: " << status.error_message
                          << "\nRobot is shut down." << std::endl;
                break;
            }
            timer_.checkpoint("status");

            // early exit if destructor has been called
            while (!has_shutdown_request() &&
                   !robot_data_->desired_action->wait_for_timeindex(t, 0.1))
            {
            }
            if (has_shutdown_request())
            {
                break;
            }

            Action desired_action = (*robot_data_->desired_action)[t];
            timer_.checkpoint("get action");

            Action applied_action = robot_driver_->apply_action(desired_action);
            timer_.checkpoint("apply action");

            robot_data_->applied_action->append(applied_action);
            timer_.checkpoint("append applied action");

            if (t % 5000 == 0 && t > 0)
            {
                timer_.print_statistics();
            }
        }

        robot_driver_->shutdown();
        loop_is_running_ = false;
    }
};

}  // namespace robot_interfaces
