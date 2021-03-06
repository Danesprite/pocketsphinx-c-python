"""
Decoder Configuration
----------------------------------------------------------------------------

"""

from os import path, walk

from pocketsphinx import get_model_path, Config


class ConfigError(Exception):
    """
    Error raised if something is wrong with the decoder configuration.
    """


def set_lm_path(config, model_path=None):
    """
    This function will try to find the LM file in model_path and set the
    '-lm' argument for the given ``Config`` object.

    Only files ending with *.lm* or *.lm.bin* will be considered. An error
    will be raised if an LM file cannot be found.

    :param config: decoder configuration object
    :param model_path: path to search for the LM file. The Pocket Sphinx
        :meth:`get_model_path()` function is used if the parameter is
        unspecified.
    :type config: Config
    :type model_path: str
    :raises: ConfigError
    """
    # Use get_model_path() if model_path was not specified.
    if model_path is None:
        model_path = get_model_path()

    lm_file = None
    for (dir_path, _, file_names) in walk(model_path):
        for f in file_names:
            if f.endswith(".lm") or f.endswith(".lm.bin"):  # LM found
                lm_file = path.join(dir_path, f)
                break

    if not lm_file:
        raise ConfigError("could not find the language model file in '%s'. Please "
                          "specify the '-lm' argument manually or use a different "
                          "model path" % model_path)

    config.set_string("-lm", lm_file)


def set_hmm_and_dict_paths(config, model_path=None):
    """
    This function will try to find the HMM directory and dictionary file
    paths in *model_path* and set the '-hmm' and '-dict' arguments in the
    given ``Config`` object.

    An error will be raised if any of the paths were not found.

    :param config: decoder configuration object
    :param model_path: path to search for HMM and dictionary. The
        Pocket Sphinx :meth:`get_model_path()` function is used if the
        parameter is unspecified.
    :type config: Config
    :type model_path: str
    :raises: ConfigError
    """
    dict_file = None
    hmm_dir = None
    hmm_required_files = [
        "feat.params", "mdef", "noisedict",
        "sendump", "transition_matrices", "variances"
    ]

    # Use get_model_path() if model_path was not specified.
    if model_path is None:
        model_path = get_model_path()

    # Find the HMM directory and dictionary file within model_path
    for (dir_path, _, file_names) in walk(model_path):
        for f in file_names:
            if f.endswith(".dict"):  # dictionary found
                dict_file = path.join(dir_path, f)

        # Does this directory contain the HMM?
        valid = True
        for required_file in hmm_required_files:
            if required_file not in file_names:
                valid = False
                break

        if valid:  # HMM directory found
            hmm_dir = dir_path

    if not (hmm_dir and dict_file):
        raise ConfigError("could not find HMM directory and/or dictionary file in "
                          "'%s'. Please specify '-hmm' and '-dict' config arguments"
                          " manually or use a different model path" % model_path)
    config.set_string("-hmm", hmm_dir)
    config.set_string("-dict", dict_file)


search_arguments = ["-lm", "-jsgf", "-kws", "-keyphrase", "-fsg"]


def search_arguments_set(config):
    """
    This function returns the search arguments set for a given ``Config``
    object.

    Search arguments include:

    * *-lm* (default)
    * *-fsg*
    * *-jsgf*
    * *-keyphrase*
    * *-kws*

    :param config: decoder configuration object
    :type config: Config
    :return: list
    """
    result = []
    for arg in search_arguments:
        if config.get_string(arg):
            result.append(arg)
    return result
