package io.github.drakou111.redstonesolver;

import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.List;
import java.util.Locale;
import java.util.Optional;

/**
 * The solver bundled in the mod jar (Windows x64: redstone_solver.exe and its DLLs, see bundleSolver in
 * build.gradle), unpacked into a folder of the game directory on first start and again when the bundle changes.
 */
public final class NativeSolver {
    private static final String NATIVES = "/redstone_solver/natives/windows-x64/";
    private static final String LIST = NATIVES + "files.txt";
    private static final String CATALOG = "/redstone_solver/catalog/gate_catalog.json";
    private static final String CATALOG_NAME = "gate_catalog.json";
    private static final String EXECUTABLE = "redstone_solver.exe";
    /** Holds the fingerprint of the bundle unpacked last. */
    private static final String MARKER = ".bundled";

    private NativeSolver() {
    }

    /** Whether this jar carries a solver this system can run. */
    public static boolean isBundled() {
        return isWindows() && NativeSolver.class.getResource(LIST) != null;
    }

    /**
     * Unpacks the bundled solver into {@code dir} unless the same bundle is already there, and returns the
     * executable; empty when the jar carries none for this system.
     */
    public static Optional<Path> install(Path dir) {
        if (!isBundled()) return Optional.empty();
        List<String> lines = readList();
        if (lines.size() < 2) throw new IllegalStateException("the bundled solver list is empty");
        String fingerprint = lines.getFirst();
        Path executable = dir.resolve(EXECUTABLE);
        Path marker = dir.resolve(MARKER);
        try {
            if (Files.isRegularFile(executable) && Files.isRegularFile(marker)
                    && Files.readString(marker, StandardCharsets.UTF_8).trim().equals(fingerprint)) {
                return Optional.of(executable);
            }
            Files.createDirectories(dir);
            for (String name : lines.subList(1, lines.size())) {
                if (name.isBlank()) continue;
                if (name.contains("/") || name.contains("\\")) throw new IllegalStateException("bad bundled file " + name);
                copyResource(NATIVES + name, dir.resolve(name));
            }
            Files.writeString(marker, fingerprint + "\n", StandardCharsets.UTF_8);
            return Optional.of(executable);
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot unpack the solver into " + dir
                    + " (is the game already running it? close other game windows and try again)", e);
        }
    }

    /** Copies the bundled gate catalog into {@code library} when it has none; a catalog already there is kept. */
    public static void installCatalog(Path library) {
        if (NativeSolver.class.getResource(CATALOG) == null) return;
        Path target = library.resolve(CATALOG_NAME);
        if (Files.exists(target)) return;
        try {
            Files.createDirectories(library);
            copyResource(CATALOG, target);
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot copy the gate catalog into " + library, e);
        }
    }

    private static boolean isWindows() {
        return System.getProperty("os.name", "").toLowerCase(Locale.ROOT).startsWith("windows");
    }

    private static List<String> readList() {
        try (InputStream in = NativeSolver.class.getResourceAsStream(LIST)) {
            if (in == null) throw new IllegalStateException("no bundled solver list");
            return new String(in.readAllBytes(), StandardCharsets.UTF_8).lines().map(String::trim).toList();
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot read the bundled solver list", e);
        }
    }

    /** Writes next to the target first, so a half-written file never takes its place. */
    private static void copyResource(String resource, Path target) throws IOException {
        Path partial = target.resolveSibling(target.getFileName() + ".part");
        try (InputStream in = NativeSolver.class.getResourceAsStream(resource)) {
            if (in == null) throw new IOException("missing bundled file " + resource);
            Files.copy(in, partial, StandardCopyOption.REPLACE_EXISTING);
        }
        Files.move(partial, target, StandardCopyOption.REPLACE_EXISTING);
    }
}
