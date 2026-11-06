package io.github.drakou111.redstonesolver.solver;

/** The solver could not be started or refused a request; the message is meant for the player. */
public final class SolverException extends RuntimeException {
    public SolverException(String message) {
        super(message);
    }

    public SolverException(String message, Throwable cause) {
        super(message, cause);
    }
}
