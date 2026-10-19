/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of irqexit automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME irqexit

enum states_irqexit {
	idle_irqexit,
	rescheduling_irqexit,
	suspended_irqexit,
	state_max_irqexit,
};

#define INVALID_STATE state_max_irqexit

enum events_irqexit {
	resched_entry_irqexit,
	resched_exit_irqexit,
	switch_in_irqexit,
	switch_out_irqexit,
	event_max_irqexit,
};

enum envs_irqexit {
	hardirq_irqexit,
	irq_off_irqexit,
	owner_current_irqexit,
	env_max_irqexit,
	env_max_stored_irqexit = hardirq_irqexit,
};

_Static_assert(env_max_stored_irqexit <= MAX_HA_ENV_LEN, "Not enough slots");

struct automaton_irqexit {
	char *state_names[state_max_irqexit];
	char *event_names[event_max_irqexit];
	char *env_names[env_max_irqexit];
	unsigned char function[state_max_irqexit][event_max_irqexit];
	unsigned char initial_state;
	bool final_states[state_max_irqexit];
};

static const struct automaton_irqexit automaton_irqexit = {
	.state_names = {
		"idle",
		"rescheduling",
		"suspended",
	},
	.event_names = {
		"resched_entry",
		"resched_exit",
		"switch_in",
		"switch_out",
	},
	.env_names = {
		"hardirq",
		"irq_off",
		"owner_current",
	},
	.function = {
		{
			rescheduling_irqexit,
			INVALID_STATE,
			idle_irqexit,
			idle_irqexit,
		},
		{
			INVALID_STATE,
			idle_irqexit,
			INVALID_STATE,
			suspended_irqexit,
		},
		{
			INVALID_STATE,
			INVALID_STATE,
			rescheduling_irqexit,
			INVALID_STATE,
		},
	},
	.initial_state = idle_irqexit,
	.final_states = { 1, 0, 0 },
};
