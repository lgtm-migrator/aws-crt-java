"""
Prepare for PKCS#11 tests by configuring SoftHSM2, if it is installed.
"""

import Builder

import os
import re


class Pkcs11TestSetup(Builder.Action):
    """
    Set up this machine for running the PKCS#11 tests.
    If SoftHSM2 cannot be installed, the tests are skipped.

    This action should be run in the 'pre_build_steps' or 'build_steps' stage.
    """

    def run(self, env):
        self.env = env

        # run on arm for Raspberry Pi
        if os.uname()[4][:3] == 'arm':
            print(f"PKCS#11 on 'ARM' is not currently supported. PKCS#11 tests are disabled")
            return

        # try to install softhsm
        try:
            softhsm_install_acion = Builder.InstallPackages(['softhsm'])
            softhsm_install_acion.run(env)
        except:
            print("WARNING: softhsm could not be installed. PKCS#11 tests are disabled")
            return

        softhsm_lib = self._find_softhsm_lib()
        if softhsm_lib is None:
            print("WARNING: libsofthsm2.so not found. PKCS#11 tests are disabled")
            return

        # put SoftHSM config file and token directory under the build dir.
        softhsm2_dir = os.path.join(env.build_dir, 'softhsm2')
        conf_path = os.path.join(softhsm2_dir, 'softhsm2.conf')
        token_dir = os.path.join(softhsm2_dir, 'tokens')
        env.shell.mkdir(token_dir)
        self._setenv('SOFTHSM2_CONF', conf_path)
        with open(conf_path, 'w') as conf_file:
            conf_file.write(f"directories.tokendir = {token_dir}\n")

        # print SoftHSM version
        self._exec_softhsm2_util('--version')

        # create a token
        self._exec_softhsm2_util(
            '--init-token',
            '--free', # use any free slot
            '--label', 'my-test-token',
            '--pin', '0000',
            '--so-pin', '0000')

        # we need to figure out which slot the new token is in because:
        # 1) old versions of softhsm2-util make you pass --slot <number>
        #    (instead of accepting --token <name> like newer versions)
        # 2) newer versions of softhsm2-util reassign new tokens to crazy
        #    slot numbers (instead of simply using 0 like older versions)
        slot = self._get_token_slots()[0]

        # add private key to token
        resources_dir = '../../crt/aws-c-io/tests/resources'
        this_dir = os.path.dirname(__file__)
        resources_dir = os.path.realpath(os.path.join(this_dir, resources_dir))
        self._exec_softhsm2_util(
            '--import', os.path.join(resources_dir, 'unittests.p8'),
            '--slot', str(slot),
            '--label', 'my-test-key',
            '--id', 'BEEFCAFE', # ID is hex
            '--pin', '0000')

        # for logging's sake, print the new state of things
        self._exec_softhsm2_util('--show-slots', '--pin', '0000')

        # set env vars for tests
        self._setenv('TEST_PKCS11_LIB', softhsm_lib)
        self._setenv('TEST_PKCS11_TOKEN_LABEL', 'my-test-token')
        self._setenv('TEST_PKCS11_PIN', '0000')
        self._setenv('TEST_PKCS11_PKEY_LABEL', 'my-test-key')
        self._setenv('TEST_PKCS11_CERT_FILE', os.path.join(resources_dir, 'unittests.crt'))
        self._setenv('TEST_PKCS11_CA_FILE', os.path.join(resources_dir, 'unittests.crt'))

    def _find_softhsm_lib(self):
        """Return path to SoftHSM2 shared lib, or None if not found"""

        # note: not using `ldconfig --print-cache` to find it because
        # some installers put it in weird places where ldconfig doesn't look
        # (like in a subfolder under lib/)

        for lib_dir in ['lib64', 'lib']: # search lib64 before lib
            for base_dir in ['/usr/local', '/usr', '/',]:
                search_dir = os.path.join(base_dir, lib_dir)
                for root, dirs, files in os.walk(search_dir):
                    for file_name in files:
                        if 'libsofthsm2.so' in file_name:
                            return os.path.join(root, file_name)
        return None


    def _exec_softhsm2_util(self, *args, **kwargs):
        if not 'check' in kwargs:
            kwargs['check'] = True

        result = self.env.shell.exec('softhsm2-util', *args, **kwargs)

        # older versions of softhsm2-util (2.1.0 is a known offender)
        # return error code 0 and print the help if invalid args are passed.
        # This should be an error.
        #
        # invalid args can happen because newer versions of softhsm2-util
        # support more args than older versions, so what works on your
        # machine might not work on some ancient docker image.
        if 'Usage: softhsm2-util' in result.output:
            raise Exception('softhsm2-util failed')

        return result

    def _get_token_slots(self):
        """Return array of IDs for slots with initialized tokens"""
        token_slot_ids = []

        output = self._exec_softhsm2_util('--show-slots', quiet=True).output

        # --- output looks like ---
        #Available slots:
        #Slot 0
        #    Slot info:
        #        ...
        #        Token present:    yes
        #    Token info:
        #        ...
        #        Initialized:      yes
        current_slot = None
        current_info_block = None
        for line in output.splitlines():
            # check for start of "Slot <ID>" block
            m = re.match(r"Slot ([0-9]+)", line)
            if m:
                current_slot = int(m.group(1))
                current_info_block = None
                continue

            if current_slot is None:
                continue

            # check for start of next indented block, like "Token info"
            m = re.match(r"    ([^ ].*)", line)
            if m:
                current_info_block = m.group(1)
                continue

            if current_info_block is None:
                continue

            # if we're in token block, check for "Initialized: yes"
            if "Token info" in current_info_block:
                if re.match(r" *Initialized: *yes", line):
                    token_slot_ids.append(current_slot)

        return token_slot_ids

    def _setenv(self, var, value):
        """
        Set environment variable now,
        and ensure the environment variable is set again when tests run
        """
        self.env.shell.setenv(var, value)
        self.env.project.config['test_env'][var] = value
