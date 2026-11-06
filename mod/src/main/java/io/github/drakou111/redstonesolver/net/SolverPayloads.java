package io.github.drakou111.redstonesolver.net;

import io.github.drakou111.redstonesolver.RedstoneSolverMod;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;

/**
 * The two messages between the client (which holds the draft) and the server (which runs the
 * solver and places blocks): a name plus a JSON body, so new actions need no new packet types.
 */
public final class SolverPayloads {
    /** Large enough for a spec or layout of the biggest allowed box with every cell set. */
    public static final int MAX_JSON_BYTES = 4 * 1024 * 1024;
    private static final int MAX_NAME_LENGTH = 32;

    /** Client to server: solve, cancel, place, discard, shrink. */
    public record ToServer(String action, String json) implements CustomPacketPayload {
        public static final Type<ToServer> TYPE = new Type<>(
                Identifier.fromNamespaceAndPath(RedstoneSolverMod.MOD_ID, "to_server"));
        public static final StreamCodec<RegistryFriendlyByteBuf, ToServer> CODEC = StreamCodec.composite(
                ByteBufCodecs.stringUtf8(MAX_NAME_LENGTH), ToServer::action,
                ByteBufCodecs.stringUtf8(MAX_JSON_BYTES), ToServer::json, ToServer::new);

        @Override
        public Type<ToServer> type() {
            return TYPE;
        }
    }

    /** Server to client: progress, result, placed, error. */
    public record ToClient(String event, String json) implements CustomPacketPayload {
        public static final Type<ToClient> TYPE = new Type<>(
                Identifier.fromNamespaceAndPath(RedstoneSolverMod.MOD_ID, "to_client"));
        public static final StreamCodec<RegistryFriendlyByteBuf, ToClient> CODEC = StreamCodec.composite(
                ByteBufCodecs.stringUtf8(MAX_NAME_LENGTH), ToClient::event,
                ByteBufCodecs.stringUtf8(MAX_JSON_BYTES), ToClient::json, ToClient::new);

        @Override
        public Type<ToClient> type() {
            return TYPE;
        }
    }

    private SolverPayloads() {
    }

    public static void register() {
        PayloadTypeRegistry.serverboundPlay().registerLarge(ToServer.TYPE, ToServer.CODEC, MAX_JSON_BYTES);
        PayloadTypeRegistry.clientboundPlay().registerLarge(ToClient.TYPE, ToClient.CODEC, MAX_JSON_BYTES);
    }
}
