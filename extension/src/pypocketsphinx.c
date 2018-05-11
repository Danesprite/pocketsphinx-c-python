/*
 * pypocketsphinx.c
 *
 *  Created on: 22 Sept. 2017
 *      Author: Dane Finlay
 *
 * Part of this file is based on source code from the CMU Pocket Sphinx project.
 * As such, the below copyright notice and conditions apply IN ADDITION TO the 
 * sphinxwrapper project's LICENSE file.
 *
 * ====================================================================
 * Copyright (c) 1999-2016 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */

#include "pypocketsphinx.h"

#define PS_DEFAULT_SEARCH "_default"

static PyObject *PocketSphinxError;

const arg_t cont_args_def[] = {
    POCKETSPHINX_OPTIONS,
    /* Argument file. */
    {"-argfile",
     ARG_STRING,
     NULL, 
     "Argument file giving extra arguments."},
    CMDLN_EMPTY_OPTION
};

PyObject *
PSObj_process_audio_internal(PSObj *self, PyObject *audio_data,
                             bool call_callbacks) {
    ps_decoder_t *ps = get_ps_decoder_t(self);

    if (ps == NULL)
        return NULL;

    if (!PyObject_TypeCheck(audio_data, &AudioDataType)) {
        PyErr_SetString(PyExc_TypeError, "argument or item is not an AudioData "
                        "object.");
        return NULL;
    }

    AudioDataObj *audio_data_c = (AudioDataObj *)audio_data;

    if (!audio_data_c->is_set) {
        PyErr_SetString(AudioDataError, "AudioData object is not set up properly. "
                        "Try using the result from AudioDevice.read_audio()");
        return NULL;
    }

    // Call ps_start_utt if necessary
    if (self->utterance_state == ENDED) {
        ps_start_utt(ps);
        self->utterance_state = IDLE;
    }

    ps_process_raw(ps, audio_data_c->audio_buffer, audio_data_c->n_samples, FALSE, FALSE);

    uint8 in_speech = ps_get_in_speech(ps);
    PyObject *result = Py_None; // incremented at end of function as result

    if (in_speech && self->utterance_state == IDLE) {
        self->utterance_state = STARTED;

        // Call speech_start callback if necessary
        PyObject *callback = self->speech_start_callback;
        if (call_callbacks && PyCallable_Check(callback)) {
            // NULL args means no args are required.
            PyObject *cb_result = PyObject_CallObject(callback, NULL);
            if (cb_result == NULL) {
                result = cb_result;
            }
        }
    } else if (!in_speech && self->utterance_state == STARTED) {
        /* speech -> silence transition, time to start new utterance  */
        ps_end_utt(ps);
        self->utterance_state = ENDED;
        char const *hyp = ps_get_hyp(ps, NULL);
	
        // Call the Python hypothesis callback if it is callable
        // It should have the correct number of arguments because
        // of the checks in set_hypothesis_callback
        PyObject *callback = self->hypothesis_callback;
        if (call_callbacks && PyCallable_Check(callback)) {
            PyObject *args;
            if (hyp != NULL) {
                args = Py_BuildValue("(s)", hyp);
            } else {
                Py_INCREF(Py_None);
                args = Py_BuildValue("(O)", Py_None);
            }
            
            PyObject *cb_result = PyObject_CallObject(callback, args);
            if (cb_result == NULL) {
                result = cb_result;
            }
        } else if (!call_callbacks) {
            // Return the hypothesis instead
            result = Py_BuildValue("s", hyp);
        }
    }

    Py_XINCREF(result);
    return result;
}

PyObject *
PSObj_process_audio(PSObj *self, PyObject *audio_data) {
    return PSObj_process_audio_internal(self, audio_data, true);
}

PyObject *
PSObj_batch_process(PSObj *self, PyObject *args, PyObject *kwds) {
    static char *kwlist[] = {"audio", "use_callbacks", NULL};
    PyObject *audio = NULL;

    // True by default. No need to increment this because it's only used internally.
    PyObject *use_callbacks = Py_True;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kwlist, &audio,
                                     &use_callbacks))
        return NULL;

    if (!PyBool_Check(use_callbacks)) {
        PyErr_SetString(PyExc_TypeError, "'use_callbacks' parameter must be a "
                        "boolean value.");
        return NULL;
    }

    if (audio == NULL || !PyList_Check(audio)) {
        PyErr_SetString(PyExc_TypeError, "'audio' parameter must be a list");
        return NULL;
    }

    Py_ssize_t list_size = PyList_Size(audio);
    PyObject *result;
    if (list_size == 0) {
        Py_INCREF(Py_None);
        result = Py_None;
    }

    for (Py_ssize_t i = 0; i < list_size; i++) {
        PyObject *item = PyList_GetItem(audio, i);
        if (!PyObject_TypeCheck(item, &AudioDataType)) {
            PyErr_SetString(PyExc_TypeError, "all list items must be AudioData "
                            "objects!");
            return NULL;
        }

        result = PSObj_process_audio_internal(
            self, item, use_callbacks == Py_True ? true : false);

        // Break on errors so NULL is returned
        if (result == NULL)
            break;

        // Discard the result object if callbacks are being used
        if (use_callbacks == Py_True) {
            Py_DECREF(result);
            result = Py_None;
        }
    }

    return result;
}

PyObject *
PSObj_end_utterance(PSObj *self) {
    ps_decoder_t *ps = get_ps_decoder_t(self);
    if (ps == NULL)
        return NULL;

    if (self->utterance_state != ENDED) {
        ps_end_utt(ps);
        self->utterance_state = ENDED;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject *
PSObj_set_search_internal(PSObj *self, ps_search_type search_type,
                          PyObject *args, PyObject *kwds) {
    // Set up the keyword list
    char *req_kw;
    switch (search_type) {
    case JSGF_STR:
        req_kw = "str";
        break;
    case KWS_STR:
        req_kw = "keyphrase";
        break;
    default: // everything else requires a file path
        req_kw = "path";
    }
    char *kwlist[] = {req_kw, "name", NULL};

    const char *value = NULL;
    const char *name = NULL;
    PyObject *result = Py_None; // incremented at end of function

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|s", kwlist, &value, &name))
        return NULL;
    
    ps_decoder_t *ps = get_ps_decoder_t(self);
    if (ps == NULL)
        return NULL;

    if (name == NULL)
        name = PS_DEFAULT_SEARCH;

    // TODO Do dictionary and LM checks for missing words - maybe add them using 
    // ps_add_word

    int set_result = -1;
    switch (search_type) {
    case JSGF_FILE:
        set_result = ps_set_jsgf_file(ps, name, value);
        break;
    case JSGF_STR:
        set_result = ps_set_jsgf_string(ps, name, value);
        break;
    case LM_FILE:
        set_result = ps_set_lm_file(ps, name, value);
        break;
    case FSG_FILE:
        ; // required because you cannot declare immediately after a label in C
        // Get the config used to initialise the decoder
        cmd_ln_t *config = get_cmd_ln_t(self);
        // Create a fsg model from the file and set it using the search name
        fsg_model_t *fsg = fsg_model_readfile(value, ps_get_logmath(ps),
                                              cmd_ln_float32_r(config, "-lw"));
        if (!fsg) {
            set_result = -1;
            break;
        }
	
        set_result = ps_set_fsg(ps, name, fsg);

        // This should be done whether or not ps_set_fsg fails, apparently..
        fsg_model_free(fsg);
        break;
    case KWS_FILE:
        // TODO Allow use of a Python list of keyword arguments rather than a file
        set_result = ps_set_kws(ps, name, value);
        break;
    case KWS_STR:
        set_result = ps_set_keyphrase(ps, name, value);
        break;
    }

    // Set the search if set_result is fine or set an error
    if (set_result < 0 || (ps_set_search(ps, name) < 0)) {
        PyErr_Format(PocketSphinxError, "something went wrong whilst setting up a "
                     "Pocket Sphinx search with name '%s'.", name);
        result = NULL;
    }

    // Keep the current search name up to date
    Py_XDECREF(self->search_name);
    self->search_name = Py_BuildValue("s", name);
    Py_INCREF(self->search_name);
    
    Py_XINCREF(result);
    return result;
}


PyObject *
PSObj_set_jsgf_file_search(PSObj *self, PyObject *args, PyObject *kwds) {
    return PSObj_set_search_internal(self, JSGF_FILE, args, kwds);
}

PyObject *
PSObj_set_jsgf_str_search(PSObj *self, PyObject *args, PyObject *kwds) {
    return PSObj_set_search_internal(self, JSGF_STR, args, kwds);
}

PyObject *
PSObj_set_lm_search(PSObj *self, PyObject *args, PyObject *kwds) {
    return PSObj_set_search_internal(self, LM_FILE, args, kwds);
}

PyObject *
PSObj_set_fsg_search(PSObj *self, PyObject *args, PyObject *kwds) {
    return PSObj_set_search_internal(self, FSG_FILE, args, kwds);
}

PyObject *
PSObj_set_keyphrase_search(PSObj *self, PyObject *args, PyObject *kwds) {
    return PSObj_set_search_internal(self, KWS_STR, args, kwds);
}

PyObject *
PSObj_set_keyphrases_search(PSObj *self, PyObject *args, PyObject *kwds) {
    return PSObj_set_search_internal(self, KWS_FILE, args, kwds);
}

PyObject *
PSObj_set_config_argument(PSObj *self, PyObject *args, PyObject *kwds) {
    cmd_ln_t *config = get_cmd_ln_t(self);
    if (config == NULL)
        return NULL;

    static char *kwlist[] = {"name", "value", "reinitialise", NULL};
    const char *name = NULL;
    const char *value = NULL;

    // True by default. No need to increment this because it's only used internally.
    PyObject *reinitialise = Py_True;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ss|O", kwlist, &name, &value,
                                     &reinitialise))
        return NULL;

    if (!PyBool_Check(reinitialise)) {
        PyErr_SetString(PyExc_TypeError, "'reinitialise' parameter must be a "
                        "boolean value.");
        return NULL;
    }

    // Set the named configuration argument if it exists or raise an error if it
    // doesn't or if setting the value fails
    if (cmd_ln_exists_r(config, name)) {
        config = cmd_ln_init(config, cont_args_def, false, name, value, NULL);
        if (config == NULL) {
            PyErr_Format(PyExc_ValueError, "failed to set Sphinx configuration "
                         "argument with the name '%s'.", name);
            return NULL;
        }
    } else {
        // Value doesn't exist. While this is not a Python dictionary lookup failure,
        // KeyError is an appropriate enough exception to raise here.
        PyErr_Format(PyExc_KeyError, "there is no Sphinx configuration argument "
                     "with the name '%s'.", name);
        return NULL;
    }

    if (reinitialise == Py_True) {
        ps_decoder_t *ps = get_ps_decoder_t(self);
        if (ps == NULL)
            return NULL;
        if (ps_reinit(ps, NULL) < 0) {
            PyErr_SetString(PocketSphinxError, "failed to reinitialise Pocket "
                            "Sphinx.");
            return NULL;
        }
    }

    self->config = config;

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject *
PSObj_get_config_argument(PSObj *self, PyObject *args, PyObject *kwds) {
    PyObject *result;
    static char *kwlist[] = {"name", NULL};
    const char *name = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &name))
        return NULL;

    cmd_ln_t *config = get_cmd_ln_t(self);
    ps_decoder_t *ps = get_ps_decoder_t(self);
    if (config == NULL || ps == NULL)
        return NULL;
    
    // Find the named argument because we need its type
    const arg_t *argument = NULL;
    for (size_t i = 0; i < sizeof(cont_args_def) / sizeof(const arg_t); i++) {
        if (cont_args_def[i].name != NULL &&
            strcmp(cont_args_def[i].name, name) == 0) {
            argument = &cont_args_def[i];
            break;
        }
    }

    if (argument == NULL) {
        PyErr_Format(PyExc_KeyError, "there is no Sphinx configuration argument "
                     "with the name '%s'.", name);
        return NULL;
    }
    
    anytype_t *type = cmd_ln_access_r(config, name);
    const char *str;
    
    switch (argument->type) {
    case ARG_INTEGER:
    case REQARG_INTEGER:
        result = Py_BuildValue("l", type->i);
        break;
    case ARG_FLOATING:
    case REQARG_FLOATING:
        result = Py_BuildValue("d", type->fl);
        break;
    case ARG_STRING:
    case REQARG_STRING:
        if (type->ptr == NULL)
            str = "";
        else
            str = (const char *)type->ptr;
        result = Py_BuildValue("s", str);
        break;
    case ARG_STRING_LIST:
        // Note: this doesn't appear to be used anywhere in sphinxbase or
        // pocketsphinx, so I'm not putting any more time into making it work.
        // This is loosely based on the cmd_ln_print_values_r implementation in
        // sphinxbase/cmd_ln.c
	
        /* ;
           const char **array;
           array = (const char**)type->ptr;
           if (array) {
           // Create a Python tuple of the same length containing all strings in
           // the list
           size_t length = sizeof(array) / sizeof(const char *);
           printf("length %lu ", length);
           result = PyList_New((Py_ssize_t)0);
           for (size_t i = 0; i < length; i++) {
           PyList_Append(result, Py_BuildValue("s", array[i]));
           }

           result = PyList_AsTuple(result);
           }
        */
        result = Py_BuildValue("()");
    	break;
    case ARG_BOOLEAN:
    case REQARG_BOOLEAN:
        result = Py_BuildValue("O", type->i ? Py_True : Py_False);
        break;
    default:
        result = Py_None;
    }

    Py_XINCREF(result);
    return result;
}

// Define a macro for documenting multiple search methods
#define PS_SEARCH_DOCSTRING(first_line, first_keyword_docstring)        \
    PyDoc_STR(first_line "\n"                                           \
              "Setting an already used search name will replace that "	\
              "Pocket Sphinx search.\n\n"                               \
              "Keyword arguments:\n"                                    \
              first_keyword_docstring "\n"                              \
              "name -- name of the Pocket Sphinx search to set "        \
              "(default '" PS_DEFAULT_SEARCH "')\n")

PyMethodDef PSObj_methods[] = {
    {"process_audio",
     (PyCFunction)PSObj_process_audio, METH_O,  // takes self + one argument
     PyDoc_STR(
         "Process audio from an AudioData object and call the speech_start and "
         "hypothesis callbacks where necessary.\n")},
    {"batch_process",
     (PyCFunction)PSObj_batch_process, METH_KEYWORDS | METH_VARARGS,
     PyDoc_STR(
         "Process a list of AudioData objects and return the speech hypothesis or "
         "use the decoder callbacks if use_callbacks is True.\n\n"
         "Keyword arguments:\n"
         "audio -- list of AudioData objects to process.\n"
         "use_callbacks -- whether to use the decoder callbacks or return the "
         "speech hypothesis (default True)\n")},
    {"end_utterance",
     (PyCFunction)PSObj_end_utterance, METH_NOARGS,  // takes no arguments
     PyDoc_STR(
         "End the current utterance if one was in progress.\n"
         "This method may be used, for example, to reset processing of audio via "
         "the process_audio method in the case of some sort of context change.\n")},
    {"set_jsgf_file_search",
     (PyCFunction)PSObj_set_jsgf_file_search, METH_KEYWORDS | METH_VARARGS,
     PS_SEARCH_DOCSTRING(
         "Set a Pocket Sphinx search using a JSpeech Grammar Format grammar file",
         "path -- file path to the JSGF file to use.")},
    {"set_jsgf_str_search",
     (PyCFunction)PSObj_set_jsgf_str_search, METH_KEYWORDS | METH_VARARGS,
     PS_SEARCH_DOCSTRING(
         "Set a Pocket Sphinx search using a JSpeech Grammar Format grammar string.",
         "str -- the JSGF string to use.")},
    {"set_lm_search",
     (PyCFunction)PSObj_set_lm_search, METH_KEYWORDS | METH_VARARGS,
     PS_SEARCH_DOCSTRING(
         "Set a Pocket Sphinx search using a language model file.",
         "path -- file path to the LM file to use.")},
    {"set_fsg_search",
     (PyCFunction)PSObj_set_fsg_search, METH_KEYWORDS | METH_VARARGS,
     PS_SEARCH_DOCSTRING(
         "Set a Pocket Sphinx search using a finite state grammar file.",
         "path -- file path to the FSG file to use.")},
    {"set_keyphrase_search",
     (PyCFunction)PSObj_set_keyphrase_search, METH_KEYWORDS | METH_VARARGS,
     PS_SEARCH_DOCSTRING(
         "Set a Pocket Sphinx search using a single keyphrase to listen for.",
         "keyphrase -- the keyphrase to listen for.")},
    {"set_keyphrases_search",
     (PyCFunction)PSObj_set_keyphrases_search, METH_KEYWORDS | METH_VARARGS,
     PS_SEARCH_DOCSTRING(
         "Set a Pocket Sphinx search using a file containing keyphrases to listen "
         "for.", "path -- file path to the keyphrases file to use.")},
    {"set_config_argument",
     (PyCFunction)PSObj_set_config_argument, METH_KEYWORDS | METH_VARARGS,
     PyDoc_STR(
         "Set a Sphinx decoder configuration argument.\n\n"
         "Keyword arguments:\n"
         "name -- the name of the configuration argument to set.\n"
         "value -- the new value for the configuration argument.\n"
         "reinitialise -- whether to reinitialise this decoder after setting the "
         "argument (default True).\n")},
    {"get_config_argument",
     (PyCFunction)PSObj_get_config_argument, METH_KEYWORDS | METH_VARARGS,
     PyDoc_STR(
         "Get the value of a Sphinx decoder configuration argument.\n\n"
         "Keyword arguments:\n"
         "name -- the name of the configuration argument to get.\n")},
    {NULL}  /* Sentinel */
};

PyObject *
PSObj_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    PSObj *self;

    self = (PSObj *)type->tp_alloc(type, 0);
    if (self != NULL) {
        // Set the callbacks to None initially
        // TODO Set to a lambda like 'lambda x : None' instead?
        Py_INCREF(Py_None);
        self->speech_start_callback = Py_None;
        Py_INCREF(Py_None);
        self->hypothesis_callback = Py_None;

        Py_INCREF(Py_None);
        self->search_name = Py_None;

        // Ensure pointer members are NULL
        self->ps = NULL;
        self->config = NULL;

        self->utterance_state = ENDED;
    }

    return (PyObject *)self;
}

void
PSObj_dealloc(PSObj *self) {
    Py_XDECREF(self->hypothesis_callback);
    Py_XDECREF(self->speech_start_callback);
    Py_XDECREF(self->search_name);
    
    // Deallocate the config object
    cmd_ln_t *config = self->config;
    if (config != NULL)
        cmd_ln_free_r(config);

    // Deallocate the Pocket Sphinx decoder
    ps_decoder_t *ps = self->ps;
    if (ps != NULL)
        ps_free(ps);

    // Finally free the PSObj itself
    Py_TYPE(self)->tp_free((PyObject*)self);
}

ps_decoder_t *
get_ps_decoder_t(PSObj *self) {
    ps_decoder_t *ps = self->ps;
    if (ps == NULL)
        PyErr_SetString(PyExc_ValueError, "PocketSphinx instance has no native "
                        "decoder reference");
    return ps;
}

cmd_ln_t *
get_cmd_ln_t(PSObj *self) {
    cmd_ln_t *config = self->config;
    if (config == NULL)
        PyErr_SetString(PyExc_ValueError, "PocketSphinx instance has no native "
                        "config reference");
    
    return config;
}

int
PSObj_init(PSObj *self, PyObject *args, PyObject *kwds) {
    PyObject *ps_args = NULL;
    Py_ssize_t list_size;

    static char *kwlist[] = {"ps_args", NULL};
    
    if (! PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &ps_args))
        return -1;

    if (ps_args && ps_args != Py_None) {
        if (!PyList_Check(ps_args)) {
            // Raise the exception flag and return -1
            PyErr_SetString(PyExc_TypeError, "parameter must be a list");
            return -1;
        }

        // Extract strings from Python list into a C string array and use that
        // to call init_ps_decoder_with_args
        list_size = PyList_Size(ps_args);
        char *strings[list_size];
        for (Py_ssize_t i = 0; i < list_size; i++) {
            PyObject *item = PyList_GetItem(ps_args, i);
            char *err_msg = "all list items must be strings!";
#if PY_MAJOR_VERSION >= 3
            if (!PyUnicode_Check(item)) {
                PyErr_SetString(PyExc_TypeError, err_msg);
                return -1;
            }

            strings[i] = PyUnicode_AsUTF8(item);
#else
            if (!PyString_Check(item)) {
                PyErr_SetString(PyExc_TypeError, err_msg);
                return -1;
            }
		
            strings[i] = PyString_AsString(item);
#endif
        }

        // Init a new pocket sphinx decoder or raise a PocketSphinxError and return -1
        if (!init_ps_decoder_with_args(self, list_size, strings)) {
            PyErr_SetString(PocketSphinxError, "PocketSphinx couldn't be initialised. "
                            "Is your configuration right?");
            return -1;
        }
    } else {
        // Let Pocket Sphinx use the default configuration if there aren't any arguments
        char *strings[0];
        if (!init_ps_decoder_with_args(self, 0, strings)) {
            PyErr_SetString(PocketSphinxError, "PocketSphinx couldn't be initialised "
                            "using the default configuration. Is it installed properly?");
            return -1;
        }
    }

    return 0;
}

PyObject *
PSObj_get_speech_start_callback(PSObj *self, void *closure) {
    Py_INCREF(self->speech_start_callback);
    return self->speech_start_callback;
}

PyObject *
PSObj_get_hypothesis_callback(PSObj *self, void *closure) {
    Py_INCREF(self->hypothesis_callback);
    return self->hypothesis_callback;
}

PyObject *
PSObj_get_in_speech(PSObj *self, void *closure) {
    PyObject *result = NULL;
    ps_decoder_t *ps = get_ps_decoder_t(self);
    if (ps != NULL) {
        uint8 in_speech = ps_get_in_speech(ps);
        if (in_speech)
            result = Py_True;
        else
            result = Py_False;

        Py_INCREF(result);
    }

    return result;
}

PyObject *
PSObj_get_active_search(PSObj *self, void *closure) {
    Py_INCREF(self->search_name);
    return self->search_name;
}

int
PSObj_set_speech_start_callback(PSObj *self, PyObject *value, void *closure) {
    if (value == NULL) {
        PyErr_SetString(PyExc_AttributeError, "Cannot delete the speech_start_callback "
                        "attribute.");
        return -1;
    }

    if (!PyCallable_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "value must be callable.");
        return -1;
    }

#if PY_MAJOR_VERSION == 2
    if (!assert_callable_arg_count(value, 0))
        return -1;
#endif

    Py_DECREF(self->speech_start_callback);
    Py_INCREF(value);
    self->speech_start_callback = value;

    return 0;
}

int
PSObj_set_hypothesis_callback(PSObj *self, PyObject *value, void *closure) {
    if (value == NULL) {
        PyErr_SetString(PyExc_AttributeError, "Cannot delete the "
                        "hypothesis_callback attribute.");
        return -1;
    }

    if (!PyCallable_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "value must be callable.");
        return -1;
    }

#if PY_MAJOR_VERSION == 2
    if (!assert_callable_arg_count(value, 1))
        return -1;
#endif

    Py_DECREF(self->hypothesis_callback);
    Py_INCREF(value);
    self->hypothesis_callback = value;

    return 0;
}

int
PSObj_set_active_search(PSObj *self, PyObject *value, void *closure) {
    if (value == NULL) {
        PyErr_SetString(PyExc_AttributeError, "Cannot delete the active_search "
                        "attribute.");
        return -1;
    }

    ps_decoder_t *ps = get_ps_decoder_t(self);
    if (ps == NULL)
        return -1;

    const char *new_search_name;
    const char *err_msg = "value must be a string.";

#if PY_MAJOR_VERSION >= 3
    if (!PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError, err_msg);
        return -1;
    }

    new_search_name = PyUnicode_AsUTF8(value);
#else
    if (!PyString_Check(value)) {
        PyErr_SetString(PyExc_TypeError, err_msg);
        return -1;
    }

    new_search_name = PyString_AsString(value);
#endif

    // Set the search and raise an error if something goes wrong
    if (ps_set_search(ps, new_search_name) < 0) {
        PyErr_Format(PocketSphinxError, "failed to set Pocket Sphinx search with "
                     "name '%s'. Perhaps there isn't a search with that name?",
                     new_search_name);
        return -1;
    }

    // Keep the current search name up to date
    Py_XDECREF(self->search_name);
    self->search_name = value;
    Py_INCREF(self->search_name);
    return 0;
}

PyGetSetDef PSObj_getseters[] = {
    {"speech_start_callback",
     (getter)PSObj_get_speech_start_callback,
     (setter)PSObj_set_speech_start_callback,
     "Callable object called when speech started.", NULL},
    {"hypothesis_callback",
     (getter)PSObj_get_hypothesis_callback,
     (setter)PSObj_set_hypothesis_callback,
     "Hypothesis callback called with Pocket Sphinx's hypothesis for "
     "what was said.", NULL},
    {"in_speech",
     (getter)PSObj_get_in_speech, NULL, // No setter. AttributeError is thrown on set attempt.
     // From pocketsphinx.h:
     "Checks if the last feed audio buffer contained speech.", NULL},
    {"active_search",
     (getter)PSObj_get_active_search,
     (setter)PSObj_set_active_search,
     "The name of the currently active Pocket Sphinx search.\n"
     "If the setter is passed a name with no matching Pocket Sphinx search, an "
     "error will be raised.", NULL},
    {NULL}  /* Sentinel */
};

PyTypeObject PSType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "sphinxwrapper.PocketSphinx", /* tp_name */
    sizeof(PSObj),                /* tp_basicsize */
    0,                            /* tp_itemsize */
    (destructor)PSObj_dealloc,    /* tp_dealloc */
    0,                            /* tp_print */
    0,                            /* tp_getattr */
    0,                            /* tp_setattr */
    0,                            /* tp_compare */
    0,                            /* tp_repr */
    0,                            /* tp_as_number */
    0,                            /* tp_as_sequence */
    0,                            /* tp_as_mapping */
    0,                            /* tp_hash */
    0,                            /* tp_call */
    0,                            /* tp_str */
    0,                            /* tp_getattro */
    0,                            /* tp_setattro */
    0,                            /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT |
    Py_TPFLAGS_BASETYPE,          /* tp_flags */
    "Pocket Sphinx decoder "
    "objects",                    /* tp_doc */
    0,                            /* tp_traverse */
    0,                            /* tp_clear */
    0,                            /* tp_richcompare */
    0,                            /* tp_weaklistoffset */
    0,                            /* tp_iter */
    0,                            /* tp_iternext */
    PSObj_methods,                /* tp_methods */
    0,                            /* tp_members */
    PSObj_getseters,              /* tp_getset */
    0,                            /* tp_base */
    0,                            /* tp_dict */
    0,                            /* tp_descr_get */
    0,                            /* tp_descr_set */
    0,                            /* tp_dictoffset */
    (initproc)PSObj_init,         /* tp_init */
    0,                            /* tp_alloc */
    PSObj_new,                    /* tp_new */
};

bool
init_ps_decoder_with_args(PSObj *self, int argc, char *argv[]) {
    char const *cfg; 
    ps_decoder_t *ps;
    cmd_ln_t *config = cmd_ln_parse_r(NULL, cont_args_def, argc, argv, TRUE);
    
    /* Handle argument file as -argfile. */
    if (config && (cfg = cmd_ln_str_r(config, "-argfile")) != NULL) {
        config = cmd_ln_parse_file_r(config, cont_args_def, cfg, FALSE);
    }

    if (config == NULL) {
        return false;
    }
    
    ps_default_search_args(config);
    ps = ps_init(config);

    if (ps == NULL) {
        return false;
    }
    
    // Set a pointer to the new decoder used only in C.
    self->ps = ps;

    // Retain the config for later use.
    // This claims ownership of the config struct.
    config = cmd_ln_retain(config);
    
    // Set a pointer to the config
    self->config = config;

    // Set self->search_name
    const char *name = ps_get_search(ps);
    Py_XDECREF(self->search_name);
    if (!name) {
        self->search_name = Py_None;
    } else {
        self->search_name = Py_BuildValue("s", name);
    }
    Py_INCREF(self->search_name);

    return true;
}

PyObject *
initpocketsphinx(PyObject *module) {
    // Set up the 'PocketSphinx' type
    PSType.tp_new = PSObj_new;
    if (PyType_Ready(&PSType) < 0) {
        return NULL;
    }

    Py_INCREF(&PSType);
    PyModule_AddObject(module, "PocketSphinx", (PyObject *)&PSType);

    // Define a new Python exception
    PocketSphinxError = PyErr_NewException("sphinxwrapper.PocketSphinxError",
                                           NULL, NULL);
    Py_INCREF(PocketSphinxError);

    PyModule_AddObject(module, "PocketSphinxError", PocketSphinxError);
    return module;
}

