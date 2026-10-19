/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Automatically generated C representation of irqstate automaton
 * For further information about this format, see kernel documentation:
 *   Documentation/trace/rv/deterministic_automata.rst
 */

#define MONITOR_NAME irqstate

enum states_irqstate {
	idle_irqstate,
	hardirq_from_softirq_irqstate,
	in_hardirq_irqstate,
	in_softirq_irqstate,
	state_max_irqstate,
};

#define INVALID_STATE state_max_irqstate

enum events_irqstate {
	hardirq_entry_irqstate,
	hardirq_exit_irqstate,
	softirq_entry_irqstate,
	softirq_exit_irqstate,
	softirq_resume_irqstate,
	softirq_suspend_irqstate,
	event_max_irqstate,
};

enum envs_irqstate {
	hardirq_irqstate,
	serving_softirq_irqstate,
	env_max_irqstate,
	env_max_stored_irqstate = hardirq_irqstate,
};

_Static_assert(env_max_stored_irqstate <= MAX_HA_ENV_LEN, "Not enough slots");

struct automaton_irqstate {
	char *state_names[state_max_irqstate];
	char *event_names[event_max_irqstate];
	char *env_names[env_max_irqstate];
	unsigned char function[state_max_irqstate][event_max_irqstate];
	unsigned char initial_state;
	bool final_states[state_max_irqstate];
};

static const struct automaton_irqstate automaton_irqstate = {
	.state_names = {
		"idle",
		"hardirq_from_softirq",
		"in_hardirq",
		"in_softirq",
	},
	.event_names = {
		"hardirq_entry",
		"hardirq_exit",
		"softirq_entry",
		"softirq_exit",
		"softirq_resume",
		"softirq_suspend",
	},
	.env_names = {
		"hardirq",
		"serving_softirq",
	},
	.function = {
		{
			in_hardirq_irqstate,
			INVALID_STATE,
			in_softirq_irqstate,
			INVALID_STATE,
			in_softirq_irqstate,
			INVALID_STATE,
		},
		{
			INVALID_STATE,
			in_softirq_irqstate,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
		},
		{
			INVALID_STATE,
			idle_irqstate,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
			INVALID_STATE,
		},
		{
			hardirq_from_softirq_irqstate,
			INVALID_STATE,
			INVALID_STATE,
			idle_irqstate,
			INVALID_STATE,
			idle_irqstate,
		},
	},
	.initial_state = idle_irqstate,
	.final_states = { 1, 0, 0, 0 },
};
