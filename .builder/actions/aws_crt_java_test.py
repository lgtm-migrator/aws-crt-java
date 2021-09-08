
import Builder
import os


class AWSCrtJavaTest(Builder.Action):

    def run(self, env):
        # tests must run with leak detection turned on
        env.shell.setenv('AWS_CRT_MEMORY_TRACING', '2')
        actions = []
        for i in range(100):
            if os.system("mvn -B test -DredirectTestOutputToFile=true -DforkCount=0 \
                -DrerunFailingTestsCount=5 -Daws.crt.memory.tracing=2 -Daws.crt.debugnative=true \
                -Daws.crt.aws_trace_log_per_test=true -DskipAfterFailureCount=1"):
                # Failed
                actions.append("exit 1")
                break
        os.system("cat log.txt")

        return Builder.Script(actions, name='aws-crt-java-test')
