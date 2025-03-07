#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

from unittest.mock import AsyncMock, patch

from click.testing import CliRunner
from later.unittest import TestCase
from openr.cli.clis import lm
from openr.cli.tests import helpers


BASE_MODULE = "openr.cli.clis.lm"
BASE_CMD_MODULE = "openr.cli.commands.lm"

from .fixtures import LM_LINKS_EXPECTED_OPENR_RIGHT_STDOUT, LM_LINKS_OPENR_RIGHT_OK


class CliLmTests(TestCase):
    def setUp(self) -> None:
        self.runner = CliRunner()

    def test_help(self) -> None:
        invoked_return = self.runner.invoke(
            lm.LMCli.lm,
            ["--help"],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)

    @patch(helpers.COMMANDS_GET_OPENR_CTRL_CPP_CLIENT)
    def test_lm_links(self, mocked_openr_client: AsyncMock) -> None:
        mocked_returned_connection = helpers.get_enter_thrift_asyncmock(
            mocked_openr_client
        )
        mocked_returned_connection.getInterfaces.return_value = LM_LINKS_OPENR_RIGHT_OK
        invoked_return = self.runner.invoke(
            lm.LMLinksCli.links,
            [],
            catch_exceptions=False,
        )
        self.assertEqual(0, invoked_return.exit_code)
        self.assertEqual(LM_LINKS_EXPECTED_OPENR_RIGHT_STDOUT, invoked_return.stdout)
