/** @file ksession_parse.c
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <faux/list.h>
#include <faux/argv.h>
#include <faux/error.h>
#include <klish/khelper.h>
#include <klish/kview.h>
#include <klish/kscheme.h>
#include <klish/kpath.h>
#include <klish/kpargv.h>
#include <klish/kexec.h>
#include <klish/ksession.h>


static bool_t ksession_validate_arg(kentry_t *entry, const char *arg)
{
	const char *str = NULL;

	assert(entry);
	if (!entry)
		return BOOL_FALSE;
	assert(arg);
	if (!arg)
		return BOOL_FALSE;

	// Temporary test code that implements COMMAND i.e. it compares argument
	// to ENTRY's 'name' or 'value'. Later it will be removed by real code.
	str = kentry_value(entry);
	if (!str)
		str = kentry_name(entry);
	if (faux_str_casecmp(str, arg) == 0)
			return BOOL_TRUE;

	return BOOL_FALSE;
}


static kpargv_status_e ksession_parse_arg(kentry_t *current_entry,
	faux_argv_node_t **argv_iter, kpargv_t *pargv)
{
	kentry_t *entry = current_entry;
	kentry_mode_e mode = KENTRY_MODE_NONE;
	kpargv_status_e retcode = KPARSE_INPROGRESS; // For ENTRY itself
	kpargv_status_e rc = KPARSE_NOTFOUND; // For nested ENTRYs
	faux_argv_node_t *saved_argv_iter = NULL;
	kpargv_purpose_e purpose = KPURPOSE_NONE;

	assert(current_entry);
	if (!current_entry)
		return KPARSE_ERROR;
	assert(argv_iter);
	if (!argv_iter)
		return KPARSE_ERROR;
	assert(pargv);
	if (!pargv)
		return KPARSE_ERROR;

	purpose = kpargv_purpose(pargv); // Purpose of parsing

	// Is entry candidate to resolve current arg?
	// Container can't be a candidate.
	if (!kentry_container(entry)) {
		const char *current_arg = NULL;

//printf("arg: %s, entry: %s\n", *argv_iter ? faux_argv_current(*argv_iter) : "<empty>",
//	kentry_name(entry));

		// When purpose is COMPLETION or HELP then fill completion list.
		// Additionally if it's last continuable argument then lie to
		// engine: make all last arguments NOTFOUND. It's necessary to walk
		// through all variants to gether all completions.
		if ((KPURPOSE_COMPLETION == purpose) ||
			(KPURPOSE_HELP == purpose)) {
			if (!*argv_iter) {
				// That's time to add entry to completions list.
				if (!kpargv_continuable(pargv))
					kpargv_add_completions(pargv, entry);
				return KPARSE_INCOMPLETED;
			} else {
				// Add entry to completions if it's last incompleted arg.
				if (faux_argv_is_last(*argv_iter) &&
					kpargv_continuable(pargv)) {
					kpargv_add_completions(pargv, entry);
					return KPARSE_NOTFOUND;
				}
			}
		}

		// If all arguments are resolved already then return INCOMPLETED
		if (!*argv_iter)
			return KPARSE_INCOMPLETED;

		// Validate argument
		current_arg = faux_argv_current(*argv_iter);
		if (ksession_validate_arg(entry, current_arg)) {
			kparg_t *parg = kparg_new(entry, current_arg);
			kpargv_add_pargs(pargv, parg);
			// Command is an ENTRY with ACTIONs or NAVigation
			if (kentry_actions_len(entry) > 0)
				kpargv_set_command(pargv, entry);
			faux_argv_each(argv_iter); // Next argument
			retcode = KPARSE_INPROGRESS;
		} else {
			// It's not a container and is not validated so
			// no chance to find anything here.
			return KPARSE_NOTFOUND;
		}
	}

	// ENTRY has no nested ENTRYs so return
	if (kentry_entrys_is_empty(entry))
		return retcode;

	// Walk through the nested entries:
	saved_argv_iter = *argv_iter;

	// EMPTY mode
	mode = kentry_mode(entry);
	if (KENTRY_MODE_EMPTY == mode)
		return retcode;

	// SWITCH mode
	// Entries within SWITCH can't has 'min'/'max' else than 1.
	// So these attributes will be ignored. Note SWITCH itself can have
	// 'min'/'max'.
	if (KENTRY_MODE_SWITCH == mode) {
		kentry_entrys_node_t *iter = kentry_entrys_iter(entry);
		kentry_t *nested = NULL;

		while ((nested = kentry_entrys_each(&iter))) {
			// Ignore entries with non-COMMON purpose.
			if (kentry_purpose(nested) != KENTRY_PURPOSE_COMMON)
				continue;
//printf("SWITCH arg: %s, entry %s\n", *argv_iter ? faux_argv_current(*argv_iter) : "<empty>", kentry_name(nested));
			rc = ksession_parse_arg(nested, argv_iter, pargv);
//printf("%s\n", kpargv_status_decode(rc));
			// If some arguments was consumed then we will not check
			// next SWITCH's entries in any case.
			if (saved_argv_iter != *argv_iter)
				break;
			// Try next entries if current status is NOTFOUND.
			// The INCOMPLETED status is for completion list. In this
			// case all next statuses will be INCOMPLETED too.
			if ((rc != KPARSE_NOTFOUND) && (rc != KPARSE_INCOMPLETED))
				break;
		}

	// SEQUENCE mode
	} else if (KENTRY_MODE_SEQUENCE == mode) {
		kentry_entrys_node_t *iter = kentry_entrys_iter(entry);
		kentry_entrys_node_t *saved_iter = iter;
		kentry_t *nested = NULL;

		while ((nested = kentry_entrys_each(&iter))) {
			kpargv_status_e nrc = KPARSE_NOTFOUND;
			size_t num = 0;
			size_t min = kentry_min(nested);

			// Ignore entries with non-COMMON purpose.
			if (kentry_purpose(nested) != KENTRY_PURPOSE_COMMON)
				continue;
			// Filter out double parsing for optional entries.
			if (kpargv_entry_exists(pargv, nested))
				continue;
			// Try to match argument and current entry
			// (from 'min' to 'max' times)
			for (num = 0; num < kentry_max(nested); num++) {
//printf("SEQ arg: %s, entry %s\n", *argv_iter ? faux_argv_current(*argv_iter) : "<empty>", kentry_name(nested));
				nrc = ksession_parse_arg(nested, argv_iter, pargv);
//printf("%s\n", kpargv_status_decode(nrc));
				if (nrc != KPARSE_INPROGRESS)
					break;
			}
			// All errors will break the loop
			if ((KPARSE_ERROR == nrc) ||
				(KPARSE_ILLEGAL == nrc) ||
				(KPARSE_NONE == nrc)) {
				rc = nrc;
				break;
			}
			// Not found necessary number of mandatory instances
			if (num < min) {
				if (KPARSE_INPROGRESS == nrc)
					rc = KPARSE_NOTFOUND;
				else
					rc = nrc; // NOTFOUND or INCOMPLETED
				break;
			}
			// It's not an error if optional parameter is absend
			rc = KPARSE_INPROGRESS;

			// Mandatory or ordered parameter
			if ((min > 0) || kentry_order(nested))
				saved_iter = iter;

			// If optional entry is found then go back to nearest
			// non-optional (or ordered) entry to try to find
			// another optional entries.
			if ((0 == min) && (num > 0))
				iter = saved_iter;
		}
	}

	// If nested result is NOTFOUND but argument was consumed
	// within nested entries or by entry itself then whole sequence
	// is ILLEGAL.
	if ((KPARSE_NOTFOUND == rc) &&
		((saved_argv_iter != *argv_iter) || !kentry_container(entry)))
		rc = KPARSE_ILLEGAL;

	return rc;
}


kpargv_t *ksession_parse_line(ksession_t *session, const faux_argv_t *argv,
	kpargv_purpose_e purpose)
{
	faux_argv_node_t *argv_iter = NULL;
	kpargv_t *pargv = NULL;
	kpargv_status_e pstatus = KPARSE_NONE;
	kpath_levels_node_t *levels_iterr = NULL;
	klevel_t *level = NULL;
	size_t level_found = 0; // Level where command was found
	kpath_t *path = NULL;

	assert(session);
	if (!session)
		return NULL;
	assert(argv);
	if (!argv)
		return NULL;

	argv_iter = faux_argv_iter(argv);

	// Initialize kpargv_t
	pargv = kpargv_new();
	assert(pargv);
	kpargv_set_continuable(pargv, faux_argv_is_continuable(argv));
	kpargv_set_purpose(pargv, purpose);

	// Iterate levels of path from higher to lower. Note the reversed
	// iterator will be used.
	path = ksession_path(session);
	levels_iterr = kpath_iterr(path);
	level_found = kpath_len(path);
	while ((level = kpath_eachr(&levels_iterr))) {
		kentry_t *current_entry = klevel_entry(level);
		// Ignore entries with non-COMMON purpose. These entries are for
		// special processing and will be ignored here.
		if (kentry_purpose(current_entry) != KENTRY_PURPOSE_COMMON)
			continue;
		// Parsing
		pstatus = ksession_parse_arg(current_entry, &argv_iter, pargv);
		if (pstatus != KPARSE_NOTFOUND)
			break;
		// NOTFOUND but some args were parsed.
		// When it's completion for first argument (that can be continued)
		// len == 0 and engine will search for completions on higher
		// levels of path.
		if (kpargv_pargs_len(pargv) > 0)
			break;
		level_found--;
	}
	// Save last argument
	if (argv_iter)
		kpargv_set_last_arg(pargv, faux_argv_current(argv_iter));
	// It's a higher level of parsing, so some statuses can have different
	// meanings
	if (KPARSE_NONE == pstatus)
		pstatus = KPARSE_ERROR; // Strange case
	else if (KPARSE_INPROGRESS == pstatus) {
		if (NULL == argv_iter) // All args are parsed
			pstatus = KPARSE_OK;
		else
			pstatus = KPARSE_ILLEGAL; // Additional not parsable args
	} else if (KPARSE_NOTFOUND == pstatus)
		pstatus = KPARSE_ILLEGAL; // Unknown command
	// If no ACTIONs were found i.e. command was not found
	if ((KPARSE_OK == pstatus) && !kpargv_command(pargv))
		pstatus = KPARSE_NOACTION;

	kpargv_set_status(pargv, pstatus);
	kpargv_set_level(pargv, level_found);

	return pargv;
}


// Delimeter of commands is '|' (pipe)
faux_list_t *ksession_split_pipes(const char *raw_line, faux_error_t *error)
{
	faux_list_t *list = NULL;
	faux_argv_t *argv = NULL;
	faux_argv_node_t *argv_iter = NULL;
	faux_argv_t *cur_argv = NULL; // Current argv
	const char *delimeter = "|";
	const char *arg = NULL;

	assert(raw_line);
	if (!raw_line)
		return NULL;

	// Split raw line to arguments
	argv = faux_argv_new();
	assert(argv);
	if (!argv)
		return NULL;
	if (faux_argv_parse(argv, raw_line) < 0) {
		faux_argv_free(argv);
		return NULL;
	}

	list = faux_list_new(FAUX_LIST_UNSORTED, FAUX_LIST_NONUNIQUE,
		NULL, NULL, (void (*)(void *))faux_argv_free);
	assert(list);
	if (!list) {
		faux_argv_free(argv);
		return NULL;
	}

	argv_iter = faux_argv_iter(argv);
	cur_argv = faux_argv_new();
	assert(cur_argv);
	while ((arg = faux_argv_each(&argv_iter))) {
		if (strcmp(arg, delimeter) == 0) {
			// End of current line (from "|" to "|")
			// '|' in a first position is an error
			if (faux_argv_len(cur_argv) == 0) {
				faux_argv_free(argv);
				faux_list_free(list);
				faux_error_sprintf(error, "The pipe '|' can't "
					"be at the first position");
				return NULL;
			}
			// Add argv to argv's list
			faux_list_add(list, cur_argv);
			cur_argv = faux_argv_new();
			assert(cur_argv);
		} else {
			faux_argv_add(cur_argv, arg);
		}
	}

	// Continuable flag is usefull for last argv
	faux_argv_set_continuable(cur_argv, faux_argv_is_continuable(argv));
	// Empty cur_argv is not an error. It's usefull for completion and help.
	// But empty cur_argv and continuable is abnormal.
	if ((faux_argv_len(cur_argv) == 0) &&
		faux_argv_is_continuable(cur_argv)) {
		faux_argv_free(argv);
		faux_list_free(list);
		faux_error_sprintf(error, "The pipe '|' can't "
			"be the last argument");
		return NULL;
	}
	faux_list_add(list, cur_argv);

	faux_argv_free(argv);

	return list;
}


// All components except last one must be legal for execution but last
// component must be parsed for completion.
// Completion is a "back-end" operation so it doesn't need detailed error
// reporting.
kpargv_t *ksession_parse_for_completion(ksession_t *session,
	const char *raw_line)
{
	faux_list_t *split = NULL;
	faux_list_node_t *iter = NULL;
	kpargv_t *pargv = NULL;

	assert(session);
	if (!session)
		return NULL;
	assert(raw_line);
	if (!raw_line)
		return NULL;

	// Split raw line (with '|') to components
	split = ksession_split_pipes(raw_line, NULL);
	if (!split || (faux_list_len(split) < 1)) {
		faux_list_free(split);
		return NULL;
	}

	iter = faux_list_head(split);
	while (iter) {
		faux_argv_t *argv = (faux_argv_t *)faux_list_data(iter);
		if (iter == faux_list_tail(split)) { // Last item
			pargv = ksession_parse_line(session, argv,
				KPURPOSE_COMPLETION);
			if (!pargv) {
				faux_list_free(split);
				return NULL;
			}
		} else { // Non-last item
			pargv = ksession_parse_line(session, argv,
				KPURPOSE_EXEC);
			// All non-last components must be ready for execution
			if (!pargv || kpargv_status(pargv) != KPARSE_OK) {
				kpargv_free(pargv);
				faux_list_free(split);
				return NULL;
			}
		}
		iter = faux_list_next_node(iter);
	}

	faux_list_free(split);

	return pargv;
}


kexec_t *ksession_parse_for_exec(ksession_t *session, const char *raw_line,
	faux_error_t *error)
{
	faux_list_t *split = NULL;
	faux_list_node_t *iter = NULL;
	kpargv_t *pargv = NULL;
	kexec_t *exec = NULL;

	assert(session);
	if (!session)
		return NULL;
	assert(raw_line);
	if (!raw_line)
		return NULL;

	// Split raw line (with '|') to components
	split = ksession_split_pipes(raw_line, error);
	if (!split || (faux_list_len(split) < 1)) {
		faux_list_free(split);
		return NULL;
	}

	// Create exec list
	exec = kexec_new();
	assert(exec);
	if (!exec) {
		faux_list_free(split);
		return NULL;
	}

	iter = faux_list_head(split);
	while (iter) {
		faux_argv_t *argv = (faux_argv_t *)faux_list_data(iter);
		kcontext_t *context = NULL;

		pargv = ksession_parse_line(session, argv, KPURPOSE_EXEC);
		// All components must be ready for execution
		if (!pargv) {
			kpargv_free(pargv);
			faux_list_free(split);
			return NULL;
		}
		if (kpargv_status(pargv) != KPARSE_OK) {
			faux_error_sprintf(error, "%s",
				kpargv_status_str(pargv));
			kpargv_free(pargv);
			faux_list_free(split);
			return NULL;
		}
		// Only the first component can have 'restore=true' attribute
		if ((iter != faux_list_head(split)) &&
			kentry_restore(kpargv_command(pargv))) {
			faux_error_sprintf(error, "The command \"%s\" "
				"can't be destination of pipe",
				kentry_name(kpargv_command(pargv)));
			kpargv_free(pargv);
			faux_list_free(split);
			return NULL;
		}

		// Fill the kexec_t
		context = kcontext_new(KCONTEXT_PLUGIN_ACTION);
		assert(context);
		kcontext_set_pargv(context, pargv);
		kexec_add_contexts(exec, context);

		// Next component
		iter = faux_list_next_node(iter);
	}

	faux_list_free(split);

	return exec;
}
