package io.github.drakou111.redstonesolver;

import java.io.IOException;
import java.io.Reader;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

/** Settings from config/redstone_solver.properties; the file is created with defaults when missing. */
public record ModConfig(Path solverExecutable) {
    public static final String FILE_NAME = "redstone_solver.properties";
    /** Set by the development runs (see build.gradle) so they use the solver from this repository. */
    public static final String EXECUTABLE_PROPERTY = "redstone.solver.exe";

    private static final String KEY_EXECUTABLE = "solver_exe";
    private static final String TEMPLATE = """
            # Path to redstone_solver.exe (build it with: cmake --build --preset release)
            solver_exe=
            """;

    public static ModConfig load(Path configDir, Path gameDir) {
        Path file = configDir.resolve(FILE_NAME);
        Properties properties = new Properties();
        try {
            if (Files.exists(file)) {
                try (Reader reader = Files.newBufferedReader(file, StandardCharsets.UTF_8)) {
                    properties.load(reader);
                }
            } else {
                Files.createDirectories(configDir);
                Files.writeString(file, TEMPLATE, StandardCharsets.UTF_8);
            }
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot read " + file, e);
        }

        String configured = properties.getProperty(KEY_EXECUTABLE, "").trim();
        String override = System.getProperty(EXECUTABLE_PROPERTY, "").trim();
        Path executable;
        if (!override.isEmpty()) executable = Path.of(override);
        else if (!configured.isEmpty()) executable = Path.of(configured);
        else executable = gameDir.resolve("redstone_solver").resolve("redstone_solver.exe");

        return new ModConfig(executable.toAbsolutePath().normalize());
    }
}
