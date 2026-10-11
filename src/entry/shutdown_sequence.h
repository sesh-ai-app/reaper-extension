// The process teardown order (requirement 1.2).
//
// Moved here from `ui/ui_host.h`, which is where it was declared because the UI Host
// was the first component that had to refuse on an out-of-order teardown. That header
// said the plugin entry is the natural long-term home and that moving it would be a
// rename rather than a redesign. This is the rename: not one line of the semantics
// below changed, and `sesh_ai::ui::ShutdownSequence` still names this type, because
// `ui_host.h` pulls it back into its own namespace.
//
// It lives in its own header rather than in `plugin_entry.h` because of the direction
// of the dependency. `ui_host.h` has to see this type — `UiHost::shut_down` takes it —
// and `plugin_entry.h` has to see the UI Host, since the entry is what drives the
// sequence through it. Putting the sequence in `plugin_entry.h` would close that into a
// cycle. A third header both can include is the only arrangement that is not one.
//
// ---------------------------------------------------------------------------
// Why this order and not another
//
// Unregister the timer, destroy the queues, shut down CEF, close the Socket.IO
// connection, exit. Out of order, REAPER crashes on quit, which producers report as
// Sesh losing their session.
//
//   - **The timer first.** A tick is the one place the extension touches REAPER's C
//     API and the one place it publishes to CEF. While it is registered, every later
//     step is racing a tick that can reach the thing being torn down.
//   - **The queues second.** An envelope still queued once the timer is gone has no
//     destination; draining nothing is fine, but a queue destroyed while a handler
//     still holds work is not.
//   - **CEF third.** Both a tick and a queued envelope outlive it otherwise, and CEF's
//     helper processes survive an abrupt exit, so its shutdown has to complete while
//     the process is still around to wait for it.
//   - **The socket fourth.** Closing it earlier turns the normal case into the
//     reconnect path, and anything still being drained loses its reply.
//   - **The process last**, which is the only step with nothing after it to protect.
//
// Each step is claimed from the sequence *before* the irreversible work it names, so a
// violation is caught while the thing it would have broken is still intact. The UI Host
// proves a component can refuse its own step; `PluginEntry::shut_down` drives all five.

#ifndef SESH_AI_ENTRY_SHUTDOWN_SEQUENCE_H
#define SESH_AI_ENTRY_SHUTDOWN_SEQUENCE_H

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sesh_ai::entry {

	enum class ShutdownStep {
		unregister_timer,
		destroy_queues,
		shut_down_cef,
		close_socket_io_connection,
		exit_extension
	};

	inline constexpr std::string_view describe(ShutdownStep step)
	{
		switch (step) {
		case ShutdownStep::unregister_timer:
			return "unregister the timer";
		case ShutdownStep::destroy_queues:
			return "destroy the queues";
		case ShutdownStep::shut_down_cef:
			return "shut down cef";
		case ShutdownStep::close_socket_io_connection:
			return "close the socket.io connection";
		case ShutdownStep::exit_extension:
			return "exit";
		}

		return "unknown step";
	}

	struct ShutdownViolation {
		ShutdownStep attempted_step = ShutdownStep::unregister_timer;

		// The step that was required instead. Absent when the sequence was already
		// complete and something tried a sixth step.
		bool sequence_was_complete = false;
		ShutdownStep expected_step = ShutdownStep::unregister_timer;

		std::string description;
	};

	// The teardown order, as a value that refuses to be walked out of order.
	//
	// Each step is claimed before the irreversible work it names is done, so a
	// violation is caught while the thing it would have broken is still intact. Once
	// broken the sequence stays broken: a later correct step must not make a wrong
	// earlier one look like it never happened.
	class ShutdownSequence {
	public:
		static constexpr std::array<ShutdownStep, 5> required_order()
		{
			return {
				ShutdownStep::unregister_timer,
				ShutdownStep::destroy_queues,
				ShutdownStep::shut_down_cef,
				ShutdownStep::close_socket_io_connection,
				ShutdownStep::exit_extension
			};
		}

		bool completed() const noexcept
		{
			return next_step_index_ >= required_order().size();
		}

		bool broken() const noexcept
		{
			return !violations_.empty();
		}

		// Only meaningful while the sequence is incomplete.
		ShutdownStep next_required_step() const
		{
			if (completed()) {
				return ShutdownStep::exit_extension;
			}

			return required_order()[next_step_index_];
		}

		bool is_next(ShutdownStep step) const
		{
			return !completed() && next_required_step() == step;
		}

		// False when `step` is not the next one required — including a repeat of a step
		// already taken and anything after the sequence is complete. The caller must not
		// proceed: an out-of-order teardown crashes REAPER on quit.
		bool record(ShutdownStep step)
		{
			if (!is_next(step)) {
				ShutdownViolation violation;
				violation.attempted_step = step;
				violation.sequence_was_complete = completed();

				violation.description.append("teardown step '")
					.append(describe(step))
					.append("' was attempted ");

				if (completed()) {
					violation.description.append("after the sequence had completed");
				} else {
					violation.expected_step = next_required_step();
					violation.description.append("but '")
						.append(describe(violation.expected_step))
						.append("' was required next");
				}

				violations_.push_back(std::move(violation));

				return false;
			}

			taken_steps_.push_back(step);
			++next_step_index_;

			return true;
		}

		const std::vector<ShutdownStep>& taken_steps() const noexcept
		{
			return taken_steps_;
		}

		const std::vector<ShutdownViolation>& violations() const noexcept
		{
			return violations_;
		}

	private:
		std::size_t next_step_index_ = 0;
		std::vector<ShutdownStep> taken_steps_;
		std::vector<ShutdownViolation> violations_;
	};

}

#endif
