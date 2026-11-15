package io.github.drakou111.redstonesolver.solver;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.jupiter.api.Test;

class SolverClientTest {
    @Test
    void namesTheCauseOfCommonWindowsExitCodes() {
        assertThat(SolverClient.describeExit(0xC0000017)).contains("out of memory").contains("0xC0000017");
        assertThat(SolverClient.describeExit(3)).contains("aborted");
        assertThat(SolverClient.describeExit(0xC0000005)).contains("access violation");
        assertThat(SolverClient.describeExit(1)).contains("antivirus");
        assertThat(SolverClient.describeExit(42)).contains("unexpectedly").contains("0x0000002A");
    }
}
