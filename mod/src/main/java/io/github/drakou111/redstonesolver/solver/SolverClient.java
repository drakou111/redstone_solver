package io.github.drakou111.redstonesolver.solver;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import com.google.gson.JsonParser;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.lang.System.Logger;
import java.lang.System.Logger.Level;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.SecureRandom;
import java.time.Duration;
import java.util.HexFormat;
import java.util.Optional;
import java.util.List;
import java.util.ArrayList;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Runs {@code redstone_solver --serve} as a child process and calls its HTTP API. */
public final class SolverClient implements AutoCloseable {
    private static final Logger LOGGER = System.getLogger(SolverClient.class.getName());
    private static final Duration STARTUP_TIMEOUT = Duration.ofSeconds(15);
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(60);
    private static final Pattern SERVE_URL = Pattern.compile("^(http://[^/]+)/\\?token=(\\S+)$");
    private static final int TOKEN_BYTES = 16;

    private final Process process;
    private final String baseUrl;
    private final String token;
    private final HttpClient http;

    private SolverClient(Process process, String baseUrl, String token) {
        this.process = process;
        this.baseUrl = baseUrl;
        this.token = token;
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
        drainToLog(new BufferedReader(new InputStreamReader(process.getErrorStream(), StandardCharsets.UTF_8)),
                "solver stderr");

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
            String code = exitCode(process);
            process.destroy();
            throw new SolverException("The solver exited at startup (exit code " + code + ")");
        }
        Matcher matcher = SERVE_URL.matcher(line.trim());
        if (!matcher.matches()) {
            process.destroy();
            throw new SolverException("Unexpected solver startup line: " + line);
        }
        drainToLog(stdout, "solver stdout");
        LOGGER.log(Level.INFO, "Solver started at {0}", matcher.group(1));
        return new SolverClient(process, matcher.group(1), matcher.group(2));
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

    private static void drainToLog(BufferedReader lines, String name) {
        Thread.ofVirtual().name(name).start(() -> {
            try (BufferedReader reader = lines) {
                String line;
                while ((line = reader.readLine()) != null) LOGGER.log(Level.DEBUG, "{0}: {1}", name, line);
            } catch (IOException e) {
                LOGGER.log(Level.DEBUG, name + " closed: " + e.getMessage());
            }
        });
    }

    private static String exitCode(Process process) {
        try {
            return process.waitFor(1, TimeUnit.SECONDS) ? String.valueOf(process.exitValue()) : "still running";
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return "unknown";
        }
    }
}
