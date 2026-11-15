package io.github.drakou111.redstonesolver.solver;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import com.google.gson.JsonParser;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.SecureRandom;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HexFormat;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/** Runs {@code redstone_solver --serve} as a child process and calls its HTTP API. */
public final class SolverClient implements AutoCloseable {
    private static final Logger LOGGER = LoggerFactory.getLogger(SolverClient.class);
    private static final Duration STARTUP_TIMEOUT = Duration.ofSeconds(15);
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(60);
    private static final Pattern SERVE_URL = Pattern.compile("^(http://[^/]+)/\\?token=(\\S+)$");
    private static final int TOKEN_BYTES = 16;
    /** The last stderr lines kept for the error message when the solver dies. */
    private static final int KEPT_STDERR_LINES = 5;
    // Windows NTSTATUS codes a crashed native process exits with.
    private static final int STATUS_ACCESS_VIOLATION = 0xC0000005;
    private static final int STATUS_NO_MEMORY = 0xC0000017;
    private static final int STATUS_STACK_OVERFLOW = 0xC00000FD;
    private static final int STATUS_STACK_BUFFER_OVERRUN = 0xC0000409;
    private static final int STATUS_ABORT = 3;

    private final Process process;
    private final String baseUrl;
    private final String token;
    private final HttpClient http;
    private final Deque<String> lastErrors;

    private SolverClient(Process process, String baseUrl, String token, Deque<String> lastErrors) {
        this.process = process;
        this.baseUrl = baseUrl;
        this.token = token;
        this.lastErrors = lastErrors;
        this.http = HttpClient.newBuilder().connectTimeout(REQUEST_TIMEOUT).build();
    }

    /** Starts the solver and waits until it serves requests. */
    public static SolverClient start(Path executable) {
        return start(executable, Optional.empty());
    }

    /** Starts the solver; with {@code library}, it keeps verified circuits there between sessions. */
    public static SolverClient start(Path executable, Optional<Path> library) {
        Objects.requireNonNull(executable, "executable");
        if (!Files.isRegularFile(executable))
            throw new SolverException("The solver is not at " + executable
                    + ". Build it (cmake --build --preset release) or set solver_exe in config/redstone_solver.properties.");

        byte[] secret = new byte[TOKEN_BYTES];
        new SecureRandom().nextBytes(secret);
        String token = HexFormat.of().formatHex(secret);
        Process process;
        try {
            List<String> command = new ArrayList<>(List.of(executable.toString(), "--serve", "--token", token));
            library.ifPresent(dir -> command.addAll(List.of("--library", dir.toAbsolutePath().toString())));
            process = new ProcessBuilder(command)
                    .directory(executable.toAbsolutePath().getParent().toFile())
                    .start();
        } catch (IOException e) {
            throw new SolverException("Cannot start the solver at " + executable + ": " + e.getMessage(), e);
        }
        Deque<String> lastErrors = new ArrayDeque<>();
        drainStderr(new BufferedReader(new InputStreamReader(process.getErrorStream(), StandardCharsets.UTF_8)),
                lastErrors);

        BufferedReader stdout = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8));
        CompletableFuture<String> firstLine = CompletableFuture.supplyAsync(() -> {
            try {
                return stdout.readLine();
            } catch (IOException e) {
                throw new SolverException("Cannot read the solver's output: " + e.getMessage(), e);
            }
        }, runnable -> Thread.ofVirtual().name("solver-startup").start(runnable));
        String line;
        try {
            line = firstLine.get(STARTUP_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
        } catch (TimeoutException e) {
            process.destroy();
            throw new SolverException("The solver did not start within " + STARTUP_TIMEOUT.toSeconds() + " s", e);
        } catch (InterruptedException e) {
            process.destroy();
            Thread.currentThread().interrupt();
            throw new SolverException("Interrupted while starting the solver", e);
        } catch (ExecutionException e) {
            process.destroy();
            throw new SolverException("The solver failed to start: " + e.getCause().getMessage(), e.getCause());
        }
        if (line == null) {
            String reason = waitForExit(process).map(SolverClient::describeExit).orElse("it is still running");
            process.destroy();
            throw new SolverException("The solver exited at startup: " + reason + lastLines(lastErrors));
        }
        Matcher matcher = SERVE_URL.matcher(line.trim());
        if (!matcher.matches()) {
            process.destroy();
            throw new SolverException("Unexpected solver startup line: " + line);
        }
        drainStdout(stdout);
        LOGGER.info("Solver started at {}", matcher.group(1));
        return new SolverClient(process, matcher.group(1), matcher.group(2), lastErrors);
    }

    public boolean isAlive() {
        return this.process.isAlive();
    }

    /** Calls one API route; a non-2xx answer becomes a SolverException with the solver's message. */
    public JsonObject call(String route, JsonObject body) {
        HttpRequest request = HttpRequest.newBuilder(URI.create(this.baseUrl + "/api/" + route))
                .timeout(REQUEST_TIMEOUT)
                .header("Content-Type", "application/json")
                .header("X-Token", this.token)
                .POST(HttpRequest.BodyPublishers.ofString(body.toString(), StandardCharsets.UTF_8))
                .build();
        HttpResponse<String> response;
        try {
            response = this.http.send(request, HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Optional<Integer> exit = waitForExit(this.process);
            if (exit.isPresent()) {
                String message = "The solver stopped: " + describeExit(exit.get()) + lastLines(this.lastErrors);
                LOGGER.error(message);
                throw new SolverException(message, e);
            }
            throw new SolverException("The solver did not answer " + route + ": " + e.getMessage(), e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new SolverException("Interrupted while calling " + route, e);
        }

        JsonObject json;
        try {
            JsonElement parsed = JsonParser.parseString(response.body());
            if (!parsed.isJsonObject()) throw new SolverException("The solver sent no JSON object for " + route);
            json = parsed.getAsJsonObject();
        } catch (JsonParseException e) {
            throw new SolverException("The solver sent invalid JSON for " + route + ": " + e.getMessage(), e);
        }
        if (response.statusCode() / 100 != 2) {
            String message = json.has("error") ? json.get("error").getAsString() : "HTTP " + response.statusCode();
            throw new SolverException(message);
        }
        return json;
    }

    @Override
    public void close() {
        this.process.destroy();
        try {
            if (!this.process.waitFor(2, TimeUnit.SECONDS)) this.process.destroyForcibly();
        } catch (InterruptedException e) {
            this.process.destroyForcibly();
            Thread.currentThread().interrupt();
        }
    }

    /** Explains an exit code in words a player can pass on in a bug report. */
    static String describeExit(int code) {
        String hex = "exit code 0x%08X".formatted(code);
        return switch (code) {
            case STATUS_NO_MEMORY -> "it ran out of memory (" + hex + "); try a smaller box or a shorter time limit";
            case STATUS_ABORT, STATUS_STACK_BUFFER_OVERRUN ->
                    "it aborted, often from running out of memory (" + hex + "); try a smaller box";
            case STATUS_ACCESS_VIOLATION -> "it crashed (" + hex + ", access violation)";
            case STATUS_STACK_OVERFLOW -> "it crashed (" + hex + ", stack overflow)";
            case 1 -> "it was ended from outside, for example by an antivirus or the task manager (" + hex + ")";
            default -> "it exited unexpectedly (" + hex + ")";
        };
    }

    private static Optional<Integer> waitForExit(Process process) {
        try {
            return process.waitFor(1, TimeUnit.SECONDS) ? Optional.of(process.exitValue()) : Optional.empty();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return Optional.empty();
        }
    }

    private static String lastLines(Deque<String> lastErrors) {
        synchronized (lastErrors) {
            return lastErrors.isEmpty() ? "" : ". Its last output: " + String.join(" | ", lastErrors);
        }
    }

    /** The solver writes to stderr only when something goes wrong, so every line goes to the game log. */
    private static void drainStderr(BufferedReader lines, Deque<String> lastErrors) {
        Thread.ofVirtual().name("solver stderr").start(() -> {
            try (BufferedReader reader = lines) {
                String line;
                while ((line = reader.readLine()) != null) {
                    LOGGER.warn("solver: {}", line);
                    synchronized (lastErrors) {
                        if (lastErrors.size() == KEPT_STDERR_LINES) lastErrors.removeFirst();
                        lastErrors.addLast(line);
                    }
                }
            } catch (IOException e) {
                LOGGER.debug("solver stderr closed: {}", e.getMessage());
            }
        });
    }

    private static void drainStdout(BufferedReader lines) {
        Thread.ofVirtual().name("solver stdout").start(() -> {
            try (BufferedReader reader = lines) {
                String line;
                while ((line = reader.readLine()) != null) LOGGER.info("solver: {}", line);
            } catch (IOException e) {
                LOGGER.debug("solver stdout closed: {}", e.getMessage());
            }
        });
    }
}
