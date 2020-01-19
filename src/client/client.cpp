#include "client.h"

#include "gl/primitive.h"
#include "gl/gl_errors.h"
#include "input/keyboard.h"
#include "world/chunk_mesh_generation.h"
#include <SFML/Window/Mouse.hpp>
#include <common/debug.h>
#include <common/network/net_command.h>
#include <common/network/net_constants.h>
#include <thread>

#include "client_config.h"

namespace {
int findChunkDrawableIndex(const ChunkPosition &position,
                           const std::vector<ChunkDrawable> &drawables)
{
    for (int i = 0; i < static_cast<int>(drawables.size()); i++) {
        if (drawables[i].position == position) {
            return i;
        }
    }
    return -1;
}

void deleteChunkRenderable(const ChunkPosition &position,
                           std::vector<ChunkDrawable> &drawables)
{
    auto index = findChunkDrawableIndex(position, drawables);
    if (index > -1) {
        drawables[index].vao.destroy();

        // As the chunk renders need not be a sorted array, "swap and pop"
        // can be used
        // More efficent (and maybe safer) than normal deletion
        std::iter_swap(drawables.begin() + index, drawables.end() - 1);
        drawables.pop_back();
    }
}
} // namespace

Client::Client()
    : NetworkHost("Client")
{
}

bool Client::init(const ClientConfig &config, float aspect)
{
    // OpenGL stuff
    m_cube = makeCubeVertexArray(1, 2, 1);

    // Basic shader
    m_basicShader.program.create("static", "static");
    m_basicShader.program.bind();
    m_basicShader.modelLocation =
        m_basicShader.program.getUniformLocation("modelMatrix");
    m_basicShader.projectionViewLocation =
        m_basicShader.program.getUniformLocation("projectionViewMatrix");

    // Chunk shader
    m_chunkShader.program.create("chunk", "chunk");
    m_chunkShader.program.bind();
    m_chunkShader.projectionViewLocation =
        m_chunkShader.program.getUniformLocation("projectionViewMatrix");

    // Texture for the player model
    m_errorSkinTexture.create("skins/error");
    m_errorSkinTexture.bind();

    m_texturePack = config.texturePack;

    // Set up the server connection
    auto peer =
        NetworkHost::createAsClient(LOCAL_HOST, config.connectionTimeout);
    if (!peer) {
        return false;
    }
    mp_serverPeer = *peer;

    // Set player stuff
    mp_player = &m_entities[NetworkHost::getPeerId()];
    mp_player->position = {CHUNK_SIZE * 2, CHUNK_SIZE * 2 + 1, CHUNK_SIZE * 2};

    m_extCamera.entity.active = false;

    m_rawPlayerSkin = gl::loadRawImageFile("skins/" + config.skinName);
    sendPlayerSkin(m_rawPlayerSkin);

    m_projectionMatrix = glm::perspective(3.14f / 2.0f, aspect, 0.01f, 2000.0f);
    m_extCamera.projection =
        glm::perspective(3.14f / 2.0f, aspect, 0.01f, 2000.0f);
    return true;
}

void Client::handleInput(const sf::Window &window, const Keyboard &keyboard)
{
    if (!m_hasReceivedGameData) {
        return;
    }
    static auto lastMousePosition = sf::Mouse::getPosition(window);
    {
        Entity *e = m_playerCameraActive ? mp_player : &m_extCamera.entity;
        if (!m_isMouseLocked && window.hasFocus() &&
            sf::Mouse::getPosition(window).y >= 0) {
            auto change = sf::Mouse::getPosition(window) - lastMousePosition;
            e->rotation.x += static_cast<float>(change.y / 8.0f);
            e->rotation.y += static_cast<float>(change.x / 8.0f);
            sf::Mouse::setPosition(
                {(int)window.getSize().x / 2, (int)window.getSize().y / 2},
                window);
            lastMousePosition = sf::Mouse::getPosition(window);
        }
    }

    // Handle keyboard input
    float PLAYER_SPEED = 5.0f;
    if (keyboard.isKeyDown(sf::Keyboard::LControl)) {
        PLAYER_SPEED *= 10;
    }

    {
        // Handle mouse input
        auto &rotation = mp_player->rotation;
        auto &velocity = mp_player->velocity;
        if (keyboard.isKeyDown(sf::Keyboard::W)) {
            velocity += forwardsVector(rotation) * PLAYER_SPEED;
        }
        else if (keyboard.isKeyDown(sf::Keyboard::S)) {
            velocity += backwardsVector(rotation) * PLAYER_SPEED;
        }
        if (keyboard.isKeyDown(sf::Keyboard::A)) {
            velocity += leftVector(rotation) * PLAYER_SPEED;
        }
        else if (keyboard.isKeyDown(sf::Keyboard::D)) {
            velocity += rightVector(rotation) * PLAYER_SPEED;
        }
        if (keyboard.isKeyDown(sf::Keyboard::Space)) {
            velocity.y += PLAYER_SPEED * 2;
        }
        else if (keyboard.isKeyDown(sf::Keyboard::LShift)) {
            velocity.y -= PLAYER_SPEED * 2;
            std::cout << mp_player->position << std::endl;
        }
        if (rotation.x < -80.0f) {
            rotation.x = -79.9f;
        }
        else if (rotation.x > 85.0f) {
            rotation.x = 84.9f;
        }
    }

    {
        auto &rotation = m_extCamera.entity.rotation;
        auto &velocity = m_extCamera.entity.velocity;
        if (keyboard.isKeyDown(sf::Keyboard::Up)) {
            velocity += forwardsVector(rotation) * PLAYER_SPEED;
        }
        else if (keyboard.isKeyDown(sf::Keyboard::Down)) {
            velocity += backwardsVector(rotation) * PLAYER_SPEED;
        }
        if (keyboard.isKeyDown(sf::Keyboard::Left)) {
            velocity += leftVector(rotation) * PLAYER_SPEED;
        }
        else if (keyboard.isKeyDown(sf::Keyboard::Right)) {
            velocity += rightVector(rotation) * PLAYER_SPEED;
        }
        if (rotation.x < -80.0f) {
            rotation.x = -79.9f;
        }
        else if (rotation.x > 85.0f) {
            rotation.x = 84.9f;
        }
    }
}

void Client::onMouseRelease(sf::Mouse::Button button, [[maybe_unused]] int x,
                            [[maybe_unused]] int y)
{
    // Handle block removal/ block placing events

    // Create a "ray"
    Ray ray(mp_player->position, mp_player->rotation);

    // Step the ray until it hits a block/ reaches maximum length
    for (; ray.getLength() < 8; ray.step()) {
        auto rayBlockPosition = toBlockPosition(ray.getEndpoint());
        if (m_chunks.manager.getBlock(rayBlockPosition) > 0) {
            BlockUpdate blockUpdate;
            blockUpdate.block = button == sf::Mouse::Left ? 0 : 1;
            blockUpdate.position = button == sf::Mouse::Left
                                       ? rayBlockPosition
                                       : toBlockPosition(ray.getLastPoint());
            m_chunks.blockUpdates.push_back(blockUpdate);
            sendBlockUpdate(blockUpdate);
            break;
        }
    }
}

void Client::onKeyRelease(sf::Keyboard::Key key)
{
    switch (key) {
        case sf::Keyboard::L:
            m_isMouseLocked = !m_isMouseLocked;
            break;

        case sf::Keyboard::P:
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            break;

        case sf::Keyboard::F:
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            break;

        case sf::Keyboard::C:
            m_extCamera.entity.active = !m_extCamera.entity.active;
            break;

        case sf::Keyboard::R:
            m_playerCameraActive = !m_playerCameraActive;
            break;

        default:
            break;
    }
}

void Client::update(float dt)
{
    NetworkHost::tick();
    if (!m_hasReceivedGameData) {
        return;
    }

    m_extCamera.entity.position += m_extCamera.entity.velocity * dt;
    m_extCamera.entity.velocity *= 0.99 * dt;

    mp_player->position += mp_player->velocity * dt;
    mp_player->velocity *= 0.99 * dt;

    sendPlayerPosition(mp_player->position);

    // Update blocks
    for (auto &blockUpdate : m_chunks.blockUpdates) {
        auto chunkPosition = toChunkPosition(blockUpdate.position);
        m_chunks.manager.ensureNeighbours(chunkPosition);
        m_chunks.manager.setBlock(blockUpdate.position, blockUpdate.block);
        m_chunks.updates.push_back(chunkPosition);

        auto p = chunkPosition;
        auto localBlockPostion = toLocalBlockPosition(blockUpdate.position);
        if (localBlockPostion.x == 0) {
            m_chunks.updates.push_back({p.x - 1, p.y, p.z});
        }
        else if (localBlockPostion.x == CHUNK_SIZE - 1) {
            m_chunks.updates.push_back({p.x + 1, p.y, p.z});
        }

        if (localBlockPostion.y == 0) {
            m_chunks.updates.push_back({p.x, p.y - 1, p.z});
        }
        else if (localBlockPostion.y == CHUNK_SIZE - 1) {
            m_chunks.updates.push_back({p.x, p.y + 1, p.z});
        }

        if (localBlockPostion.z == 0) {
            m_chunks.updates.push_back({p.x, p.y, p.z - 1});
        }
        else if (localBlockPostion.z == CHUNK_SIZE - 1) {
            m_chunks.updates.push_back({p.x, p.y, p.z + 1});
        }
    }
    m_chunks.blockUpdates.clear();

    auto playerChunk = worldToChunkPosition(mp_player->position);
    auto distanceToPlayer = [&playerChunk](const ChunkPosition &chunkPosition) {
        return glm::abs(playerChunk.x - chunkPosition.x) +
               glm::abs(playerChunk.y - chunkPosition.y) +
               glm::abs(playerChunk.z - chunkPosition.z);
    };

    if (!m_chunks.updates.empty()) {
        // Sort chunk updates by distance if the update vector is not
        // sorted already
        if (!std::is_sorted(m_chunks.updates.begin(), m_chunks.updates.end(),
                            [&](const auto &a, const auto &b) {
                                return distanceToPlayer(a) <
                                       distanceToPlayer(b);
                            })) {
            // Remove non-unique elements
            std::unordered_set<ChunkPosition, ChunkPositionHash> updates;
            for (auto &update : m_chunks.updates) {
                updates.insert(update);
            }

            m_chunks.updates.assign(updates.cbegin(), updates.cend());

            // Sort it to find chunk mesh cloest to the player
            std::sort(m_chunks.updates.begin(), m_chunks.updates.end(),
                      [&](const auto &a, const auto &b) {
                          return distanceToPlayer(a) < distanceToPlayer(b);
                      });
        }

        if (m_noMeshingCount != m_chunks.updates.size()) {
            m_blockMeshing = false;
        }

        // Find first "meshable" chunk
        int count = 0;
        if (!m_blockMeshing) {
            m_noMeshingCount = 0;
            for (auto itr = m_chunks.updates.cbegin();
                 itr != m_chunks.updates.cend();) {
                if (m_chunks.manager.hasNeighbours(*itr)) {
                    auto &chunk = m_chunks.manager.getChunk(*itr);
                    auto buffer = makeChunkMesh(chunk, m_voxelData);
                    m_chunks.bufferables.push_back(buffer);
                    deleteChunkRenderable(*itr);
                    itr = m_chunks.updates.erase(itr);

                    // Break so that the game still runs while world is
                    // being built
                    // TODO: Work out a way to make this concurrent (aka
                    // run seperate from rest of application)
                    if (count++ > 3) {
                        break;
                    }
                }
                else {
                    m_noMeshingCount++;
                    itr++;
                }
            }
            if (m_noMeshingCount == m_chunks.updates.size()) {
                m_blockMeshing = true;
            }
        }
    }
}

void Client::render()
{
    // TODO [Hopson] Clean this up
    if (!m_hasReceivedGameData) {
        return;
    }
    // Setup matrices
    m_basicShader.program.bind();
    glm::mat4 playerProjectionView = createProjectionViewMatrix(
        mp_player->position, mp_player->rotation, m_projectionMatrix);
    glm::mat4 cameraProjectionView = createProjectionViewMatrix(
        m_extCamera.entity.position, m_extCamera.entity.rotation,
        m_extCamera.projection);

    if (m_extCamera.entity.active) {
        gl::loadUniform(m_basicShader.projectionViewLocation,
                        cameraProjectionView);
    }
    else {
        gl::loadUniform(m_basicShader.projectionViewLocation,
                        playerProjectionView);
    }

    // Update the viewing frustum for frustum culling
    m_frustum.update(playerProjectionView);

    // Render all the entities
    auto drawable = m_cube.getDrawable();
    drawable.bind();

    for (auto &ent : m_entities) {
        if (!m_extCamera.entity.active && &ent == mp_player) {
            continue;
        }
        else {
            if (ent.active) {
                if (ent.playerSkin.textureExists()) {
                    ent.playerSkin.bind();
                }
                else {
                    m_errorSkinTexture.bind();
                }

                glm::mat4 modelMatrix{1.0f};
                translateMatrix(modelMatrix, {ent.position.x, ent.position.y,
                                              ent.position.z});
                gl::loadUniform(m_basicShader.modelLocation, modelMatrix);
                drawable.draw();
            }
        }
    }

    // Render chunks
    m_chunkShader.program.bind();

    m_voxelTextures.bind();
    if (m_extCamera.entity.active) {
        gl::loadUniform(m_chunkShader.projectionViewLocation,
                        cameraProjectionView);
    }
    else {
        gl::loadUniform(m_chunkShader.projectionViewLocation,
                        playerProjectionView);
    }

    // Buffer chunks
    for (auto &chunkMesh : m_chunks.bufferables) {
        // TODO [Hopson] -> DRY this code
        if (chunkMesh.blockMesh.indicesCount > 0) {
            m_chunks.drawables.push_back({chunkMesh.blockMesh.position,
                                          chunkMesh.blockMesh.createBuffer()});
        }
        if (chunkMesh.fluidMesh.indicesCount > 0) {
            std::cout << "buffer el watero\n" << std::endl;
            m_chunks.fluidDrawables.push_back(
                {chunkMesh.fluidMesh.position,
                 chunkMesh.fluidMesh.createBuffer()});
        }
    }
    m_chunks.bufferables.clear();

    // Render them (if in view)
    for (const auto &chunk : m_chunks.drawables) {
        if (m_frustum.chunkIsInFrustum(chunk.position)) {
            chunk.vao.getDrawable().bindAndDraw();
        }
    }
    glCheck(glEnable(GL_BLEND));
    for (const auto &chunk : m_chunks.fluidDrawables) {
        if (m_frustum.chunkIsInFrustum(chunk.position)) {
            chunk.vao.getDrawable().bindAndDraw();
        }
    }
    glCheck(glDisable(GL_BLEND));
}

void Client::endGame()
{
    // Destroy all player skins
    for (auto &ent : m_entities) {
        if (ent.playerSkin.textureExists()) {
            ent.playerSkin.destroy();
        }
    }
    m_errorSkinTexture.destroy();

    m_cube.destroy();
    m_basicShader.program.destroy();
    m_chunkShader.program.destroy();
    m_voxelTextures.destroy();

    for (auto &chunk : m_chunks.drawables) {
        chunk.vao.destroy();
    }
    for (auto &chunk : m_chunks.fluidDrawables) {
        chunk.vao.destroy();
    }
    NetworkHost::disconnectFromPeer(mp_serverPeer);
}

EngineStatus Client::currentStatus() const
{
    return m_status;
}

void Client::deleteChunkRenderable(const ChunkPosition &position)
{
    ::deleteChunkRenderable(position, m_chunks.drawables);
    ::deleteChunkRenderable(position, m_chunks.fluidDrawables);
}
