"""Offline checks; never launch Codex or call GitHub/the model API."""
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import tomllib
import unittest
from unittest.mock import patch

import cpa_protocol_test as probe

TEST_BASE_URL = "https://example.invalid/cpa-test/v1"


def environment():
    return {"GITHUB_REPOSITORY": probe.REPOSITORY, "GH_REPO": probe.REPOSITORY,
            "TEST_ISSUE_NUMBER": "3", "TEST_PROTOCOL": "messages",
            "CPA_TEST_API_BASE_URL": TEST_BASE_URL, "CPA_TEST_API_KEY": "fake-model-secret",
            "GH_TOKEN": "fake-github-secret"}


class RunnerTests(unittest.TestCase):
    def test_valid_inputs(self):
        self.assertEqual(probe.validate(environment()), ("3", "messages", probe.MODELS["messages"], TEST_BASE_URL))

    def test_chat_protocol_selects_isolated_chat_model(self):
        self.assertEqual(probe.validate(dict(environment(), TEST_PROTOCOL="chat")),
                         ("3", "chat", "agnes-3.0-chat-test", TEST_BASE_URL))

    def test_rejects_main_repository_or_foreign_gh_repo(self):
        for key in ("GITHUB_REPOSITORY", "GH_REPO"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate(dict(environment(), **{key: "MaaEnd/MaaEnd"}))

    def test_rejects_invalid_issue_numbers(self):
        for number in ("0", "-1", "3; echo bad", "01", "3.1", ""):
            with self.subTest(number=number), self.assertRaises(ValueError):
                probe.validate(dict(environment(), TEST_ISSUE_NUMBER=number))

    def test_rejects_unknown_protocol(self):
        with self.assertRaises(ValueError):
            probe.validate(dict(environment(), TEST_PROTOCOL="arbitrary"))

    def test_rejects_non_test_or_unsafe_endpoint(self):
        for url in ("https://example.invalid/v1", "http://example.invalid/cpa-test/v1",
                    "https://user:password@example.invalid/cpa-test/v1", TEST_BASE_URL + "?secret=value",
                    TEST_BASE_URL + "#fragment", ""):
            with self.subTest(url=url), self.assertRaises(ValueError):
                probe.validate(dict(environment(), CPA_TEST_API_BASE_URL=url))

    def test_requires_both_credentials(self):
        for key in ("GH_TOKEN", "CPA_TEST_API_KEY"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                probe.validate(dict(environment(), **{key: ""}))

    def test_redacts_overlapping_secrets(self):
        self.assertEqual(probe.redact("token-extra token other", ["token", "token-extra", ""]),
                         "[REDACTED] [REDACTED] other")

    def test_provider_config_matches_original_wire_protocol(self):
        for model in probe.MODELS.values():
            config = tomllib.loads(probe.codex_config(model, TEST_BASE_URL))
            self.assertEqual(config["model"], model)
            self.assertEqual(config["model_reasoning_effort"], "xhigh")
            provider = config["model_providers"]["custom"]
            self.assertEqual(provider["wire_api"], "responses")
            self.assertEqual(provider["env_key"], "CODEX_API_KEY")
            self.assertEqual(provider["base_url"], TEST_BASE_URL)
            self.assertNotIn("fake-model-secret", probe.codex_config(model, TEST_BASE_URL))

    def test_prompt_targets_copy_and_permits_fork_skill_scope(self):
        prompt = probe.prompt_for("3")
        self.assertIn("https://github.com/Aliothmoon/MaaEnd/issues/3", prompt)
        self.assertNotIn("https://github.com/MaaEnd/MaaEnd/issues/", prompt)
        self.assertIn("覆盖 Skill", prompt)
        self.assertIn("不创建或更新任何 Issue/PR 评论", prompt)

    def run_fake_process(self, exit_code, answer):
        class FakeProcess:
            def __init__(self, *args, **kwargs):
                self.stdin = io.StringIO()
                self.stdout = io.StringIO('{"type":"test","text":"fake-model-secret fake-github-secret"}\n')

            def __enter__(self):
                return self

            def __exit__(self, *args):
                pass

            def wait(self):
                if answer is not None:
                    probe.ANSWER.write_text(answer, encoding="utf-8")
                return exit_code

        original = Path.cwd()
        with tempfile.TemporaryDirectory() as directory:
            try:
                os.chdir(directory)
                env = dict(environment(), RUNNER_TEMP=directory + "/runtime")
                issue = json.dumps({"number": 3, "title": "Copied issue", "body": "Log links", "html_url": "fork/3"}).encode()
                console = io.StringIO()
                with patch.dict(os.environ, env, clear=True), patch.object(probe.subprocess, "check_output",
                        side_effect=[issue, "codex-cli 0.162.1\n"]) as fetch, \
                        patch.object(probe.subprocess, "Popen", FakeProcess), contextlib.redirect_stdout(console):
                    code = probe.main()
                self.assertEqual(fetch.call_args_list[0].args[0],
                                 ["gh", "api", "repos/Aliothmoon/MaaEnd/issues/3"])
                root = Path(env["RUNNER_TEMP"]) / "cpa-protocol-artifacts"
                contents = "\n".join(p.read_text(encoding="utf-8") for p in root.iterdir()) + console.getvalue()
                self.assertNotIn("fake-model-secret", contents)
                self.assertNotIn("fake-github-secret", contents)
                return code, json.loads((root / "result.json").read_text())
            finally:
                os.chdir(original)

    def test_success_requires_answer_and_sanitizes_artifacts(self):
        code, result = self.run_fake_process(0, "Conclusion: fake-model-secret fake-github-secret")
        self.assertEqual(code, 0)
        self.assertTrue(result["answer_present"])

    def test_missing_answer_is_failure(self):
        code, result = self.run_fake_process(0, None)
        self.assertEqual(code, 1)
        self.assertFalse(result["answer_present"])

    def test_agent_failure_is_not_hidden_by_answer(self):
        code, result = self.run_fake_process(7, "Partial answer")
        self.assertEqual(code, 7)
        self.assertEqual(result["exit_code"], 7)


if __name__ == "__main__":
    unittest.main()
