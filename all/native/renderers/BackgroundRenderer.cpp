#include "BackgroundRenderer.h"
#include "components/Layers.h"
#include "components/Options.h"
#include "graphics/Bitmap.h"
#include "graphics/Shader.h"
#include "graphics/ShaderManager.h"
#include "graphics/TextureManager.h"
#include "graphics/Texture.h"
#include "graphics/ViewState.h"
#include "graphics/utils/GLContext.h"
#include "layers/Layer.h"
#include "utils/Const.h"
#include "utils/Log.h"

#include <cglib/mat.h>

namespace carto {

    BackgroundRenderer::BackgroundRenderer(const Options& options, const Layers& layers) :
        _backgroundBitmap(),
        _backgroundTex(),
        _backgroundVertices(),
        _skyBitmap(),
        _skyTex(),
        _skyVertices(),
        _backgroundCoords(),
        _backgroundNormals(),
        _backgroundTexCoords(),
        _backgroundIndices(),
        _skyCoords(),
        _skyTexCoords(),
        _shader(),
        _a_coord(0),
        _a_normal(0),
        _a_texCoord(0),
        _u_tex(0),
        _u_lightDir(0),
        _u_mvpMat(0),
        _textureManager(),
        _options(options),
        _layers(layers)
    {
    }
    
    BackgroundRenderer::~BackgroundRenderer() {
    }
    
    void BackgroundRenderer::onSurfaceCreated(const std::shared_ptr<ShaderManager>& shaderManager, const std::shared_ptr<TextureManager>& textureManager) {
        static ShaderSource shaderSource("background", &BACKGROUND_VERTEX_SHADER, &BACKGROUND_FRAGMENT_SHADER);

        _shader = shaderManager->createShader(shaderSource);

        // Get shader variables locations
        glUseProgram(_shader->getProgId());
        _u_tex = _shader->getUniformLoc("u_tex");
        _u_lightDir = _shader->getUniformLoc("u_lightDir");
        _u_mvpMat = _shader->getUniformLoc("u_mvpMat");
        _a_coord = _shader->getAttribLoc("a_coord");
        _a_normal = _shader->getAttribLoc("a_normal");
        _a_texCoord = _shader->getAttribLoc("a_texCoord");
    
        _textureManager = textureManager;
        _backgroundBitmap.reset();
        _backgroundTex.reset();
        _skyBitmap.reset();
        _skyTex.reset();
    }
    
    void BackgroundRenderer::onDrawFrame(const ViewState& viewState) {
        std::vector<std::shared_ptr<Layer> > layers = _layers.getAll();

        std::shared_ptr<Bitmap> backgroundBitmap;
        if (!layers.empty() && _options.getBackgroundBitmap() == Options::GetDefaultBackgroundBitmap()) {
            backgroundBitmap = layers.front()->getBackgroundBitmap();
        }
        if (!backgroundBitmap) {
            backgroundBitmap = _options.getBackgroundBitmap();
        }
        if (_backgroundBitmap != backgroundBitmap) {
            if (backgroundBitmap) {
                _backgroundTex = _textureManager->createTexture(backgroundBitmap, true, true);
            } else {
                _backgroundTex.reset();
            }
            _backgroundBitmap = backgroundBitmap;
        }

        std::shared_ptr<Bitmap> skyBitmap;
        if (!layers.empty()) {
            skyBitmap = layers.front()->getSkyBitmap();
        }
        if (!skyBitmap) {
            skyBitmap = _options.getSkyBitmap();
        }
        if (_skyBitmap != skyBitmap) {
            if (skyBitmap) {
                _skyTex = _textureManager->createTexture(skyBitmap, false, false);
            } else {
                _skyTex.reset();
            }
           _skyBitmap = skyBitmap;
        }
    
        if (_skyTex || _backgroundTex) {
            // Prepare for drawing
            glUseProgram(_shader->getProgId());
            // Texture
            glUniform1i(_u_tex, 0);
            glActiveTexture(GL_TEXTURE0);
            // Default lighting
            glUniform3f(_u_lightDir, 0, 0, 1);
            // Transformation matrix
            const cglib::mat4x4<float>& mvpMat = viewState.getRTESkyProjectionMat();
            glUniformMatrix4fv(_u_mvpMat, 1, GL_FALSE, mvpMat.data());
            // Coords, texCoords, colors
            glEnableVertexAttribArray(_a_coord);
            glEnableVertexAttribArray(_a_texCoord);
            glVertexAttrib3f(_a_normal, 0, 0, 1);
    
            glDepthMask(GL_FALSE);
            if (viewState.isSkyVisible()) {
                drawSky(viewState);
            }
            drawBackground(viewState);
            glDepthMask(GL_TRUE);
    
            // Disable bound arrays
            glDisableVertexAttribArray(_a_coord);
            glDisableVertexAttribArray(_a_texCoord);
        }
    
        GLContext::CheckGLError("BackgroundRenderer::onDrawFrame");
    }
    
    void BackgroundRenderer::onSurfaceDestroyed() {
        _backgroundBitmap.reset();
        _backgroundTex.reset();
        _skyBitmap.reset();
        _skyTex.reset();

        _shader.reset();
        _textureManager.reset();
    }
    
    void BackgroundRenderer::drawBackground(const ViewState& viewState) {
        if (!_backgroundTex) {
            return;
        }

        int intTwoPowZoom = 1 << static_cast<int>(viewState.getZoom());
        const cglib::vec3<double>& focusPos = viewState.getFocusPos();
        const cglib::vec3<double>& cameraPos = viewState.getCameraPos();

        // Texture
        glBindTexture(GL_TEXTURE_2D, _backgroundTex->getTexId());

        // Spherical mode?
        if (_options.getRenderProjectionMode() == RenderProjectionMode::RENDER_PROJECTION_MODE_SPHERICAL) {
            // Build sphere surface on first call
            if (_backgroundIndices.empty()) {
                BuildSphereSurface(_backgroundCoords, _backgroundNormals, _backgroundTexCoords, _backgroundIndices, SPHERE_TESSELATION_LEVELS_U, SPHERE_TESSELATION_LEVELS_V);
            }

            // Calculate coordinate transformation parameters
            float coordScale = static_cast<float>(Const::WORLD_SIZE / Const::PI);
            float backgroundScale = static_cast<float>(Const::WORLD_SIZE / viewState.getCosHalfFOVXY());
            float scale = static_cast<float>(intTwoPowZoom * 0.5f / Const::HALF_WORLD_SIZE);
            double translateOriginX = (focusPos(0) != 0 || focusPos(1) != 0 ? std::atan2(focusPos(1), focusPos(0)) / Const::PI + 1.0 : 0);
            double translateOriginY = std::asin(std::max(-1.0, std::min(1.0, focusPos(2) / cglib::length(focusPos)))) / Const::PI + 0.5;
            double translateX = translateOriginX * scale - 0.5 * scale * backgroundScale;
            double translateY = translateOriginY * scale + 0.5 * scale * backgroundScale;
            translateX -= std::floor(translateX);
            translateY -= std::floor(translateY);

            // Build vertex array
            std::size_t vertexCount = _backgroundCoords.size();
            if (vertexCount != _backgroundVertices.size() * 8) {
                _backgroundVertices.resize(vertexCount * 8);
            }
            for (std::size_t i = 0; i < vertexCount; i++) {
                _backgroundVertices[i * 8 + 0] = static_cast<float>(_backgroundCoords[i](0) * coordScale - cameraPos(0));
                _backgroundVertices[i * 8 + 1] = static_cast<float>(_backgroundCoords[i](1) * coordScale - cameraPos(1));
                _backgroundVertices[i * 8 + 2] = static_cast<float>(_backgroundCoords[i](2) * coordScale - cameraPos(2));

                _backgroundVertices[i * 8 + 3] = _backgroundNormals[i](0);
                _backgroundVertices[i * 8 + 4] = _backgroundNormals[i](1);
                _backgroundVertices[i * 8 + 5] = _backgroundNormals[i](2);

                _backgroundVertices[i * 8 + 6] = static_cast<float>(_backgroundTexCoords[i](0) * scale * backgroundScale + translateX);
                _backgroundVertices[i * 8 + 7] = static_cast<float>(_backgroundTexCoords[i](1) * scale * backgroundScale - translateY);
            }

            // Lighting
            cglib::vec3<float> lightDir = viewState.getFocusPosNormal();
            glUniform3fv(_u_lightDir, 1, lightDir.data());
            
            // Draw
            glEnableVertexAttribArray(_a_normal);
            glVertexAttribPointer(_a_coord, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), _backgroundVertices.data() + 0);
            glVertexAttribPointer(_a_normal, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), _backgroundVertices.data() + 3);
            glVertexAttribPointer(_a_texCoord, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), _backgroundVertices.data() + 6);
            glDrawElements(GL_TRIANGLES, _backgroundIndices.size(), GL_UNSIGNED_SHORT, _backgroundIndices.data());
            glDisableVertexAttribArray(_a_normal);
        } else if (_options.getRenderProjectionMode() == RenderProjectionMode::RENDER_PROJECTION_MODE_PLANAR) {
            // Calculate coordinate transformation parameters
            float backgroundScale = static_cast<float>(viewState.getFar() * 2 / viewState.getCosHalfFOVXY());
            float scale = static_cast<float>(intTwoPowZoom * 0.5f / Const::HALF_WORLD_SIZE);
            double translateX = cameraPos(0) * scale;
            double translateY = cameraPos(1) * scale;
            translateX -= std::floor(translateX);
            translateY -= std::floor(translateY);

            // Build vertex array
            std::size_t vertexCount = sizeof(PLANE_COORDS) / sizeof(float) / 3;
            if (_backgroundVertices.size() != vertexCount * 5) {
                _backgroundVertices.resize(vertexCount * 5);
            }
            for (std::size_t i = 0; i < vertexCount; i++) {
                _backgroundVertices[i * 5 + 0] = PLANE_COORDS[i * 3 + 0] * backgroundScale;
                _backgroundVertices[i * 5 + 1] = PLANE_COORDS[i * 3 + 1] * backgroundScale;
                _backgroundVertices[i * 5 + 2] = static_cast<float>(-cameraPos(2));

                _backgroundVertices[i * 5 + 3] = static_cast<float>((PLANE_TEX_COORDS[i * 2 + 0] - 0.5f) * scale * backgroundScale + translateX);
                _backgroundVertices[i * 5 + 4] = static_cast<float>((PLANE_TEX_COORDS[i * 2 + 1] - 0.5f) * scale * backgroundScale + translateY);
            }

            // Draw
            glVertexAttribPointer(_a_coord, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), _backgroundVertices.data() + 0);
            glVertexAttribPointer(_a_texCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), _backgroundVertices.data() + 3);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, vertexCount);
        }
    }
    
    void BackgroundRenderer::drawSky(const ViewState& viewState) {
        if (!_skyTex) {
            return;
        }

        // Texture
        glBindTexture(GL_TEXTURE_2D, _skyTex->getTexId());

        // Build sky procedurally
        _skyCoords.clear();
        _skyTexCoords.clear();
        if (_options.getRenderProjectionMode() == RenderProjectionMode::RENDER_PROJECTION_MODE_SPHERICAL) {
            float coordScale = static_cast<float>(Const::WORLD_SIZE / Const::PI);
            double height0 = SKY_RELATIVE_HEIGHT_SPHERICAL[0] * std::pow(2.0f, -viewState.getZoom() / SKY_HEIGHT_RAMP_SPHERICAL[0]) * coordScale;
            double height1 = SKY_RELATIVE_HEIGHT_SPHERICAL[1] * std::pow(2.0f, -viewState.getZoom() / SKY_HEIGHT_RAMP_SPHERICAL[1]) * coordScale;
            BuildSphereSky(_skyCoords, _skyTexCoords, viewState.getCameraPos(), viewState.getUpVec(), height0, height1, coordScale, SKY_TESSELATION_LEVELS);
        } else if (_options.getRenderProjectionMode() == RenderProjectionMode::RENDER_PROJECTION_MODE_PLANAR) {
            float coordScale = viewState.getFar() * SKY_SCALE_MULTIPLIER_PLANAR;
            double height0 = SKY_RELATIVE_HEIGHT_PLANAR[0] * std::pow(2.0f, -viewState.getZoom() / SKY_HEIGHT_RAMP_PLANAR[0]) * coordScale;
            double height1 = SKY_RELATIVE_HEIGHT_PLANAR[1] * std::pow(2.0f, -viewState.getZoom() / SKY_HEIGHT_RAMP_PLANAR[1]) * coordScale;
            BuildPlanarSky(_skyCoords, _skyTexCoords, viewState.getCameraPos(), viewState.getFocusPos(), viewState.getUpVec(), height0, height1, coordScale);
        }

        // Copy/pack coordinates
        std::size_t vertexCount = _skyCoords.size();
        if (_skyVertices.size() != vertexCount * 5) {
            _skyVertices.resize(vertexCount * 5);
        }
        for (std::size_t i = 0; i < vertexCount; i++) {
            _skyVertices[i * 5 + 0] = _skyCoords[i](0);
            _skyVertices[i * 5 + 1] = _skyCoords[i](1);
            _skyVertices[i * 5 + 2] = _skyCoords[i](2);

            _skyVertices[i * 5 + 3] = _skyTexCoords[i](0);
            _skyVertices[i * 5 + 4] = _skyTexCoords[i](1);
        }

        // Draw
        glVertexAttribPointer(_a_coord, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), _skyVertices.data() + 0);
        glVertexAttribPointer(_a_texCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), _skyVertices.data() + 3);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, vertexCount);
    }

    void BackgroundRenderer::BuildPlanarSky(std::vector<cglib::vec3<float> >& coords, std::vector<cglib::vec2<float> >& texCoords, const cglib::vec3<double>& cameraPos, const cglib::vec3<double>& focusPos, const cglib::vec3<double>& upVec, double height0, double height1, float coordScale) {
        cglib::vec3<double> cameraVec = focusPos - cameraPos;

        cglib::vec3<double> axis3 = cglib::unit(cglib::vec3<double>(cameraVec(0), cameraVec(1), 0));
        cglib::vec3<double> axis2 = cglib::unit(upVec);
        cglib::vec3<double> axis1 = cglib::unit(cglib::vector_product(axis3, axis2));

        for (int j = 0; j < 2; j++) {
            for (int i = 0; i < 2; i++) {
                double x = (i - 0.5) * coordScale;
                double y = height0 + j * (height1 - height0);
                double z = 0.5 * coordScale;
                coords.emplace_back(cglib::vec3<float>::convert(axis1 * x + axis2 * y + axis3 * z));
                texCoords.emplace_back(0.5f, j);
            }
        }
    }

    void BackgroundRenderer::BuildSphereSky(std::vector<cglib::vec3<float> >& coords, std::vector<cglib::vec2<float> >& texCoords, const cglib::vec3<double>& cameraPos, const cglib::vec3<double>& upVec, double height0, double height1, float coordScale, int tesselate) {
        int vertexCount = (tesselate + 1) * 2;
        coords.reserve(vertexCount);
        texCoords.reserve(vertexCount);

        cglib::vec3<double> axis1a = cglib::unit(cglib::vector_product(cglib::vector_product(cameraPos, upVec), cameraPos));
        cglib::vec3<double> axis2a = cglib::unit(cglib::vector_product(cameraPos, axis1a));

        cglib::vec3<double> axis1b = cglib::unit(upVec);
        cglib::vec3<double> axis2b = cglib::unit(cglib::vector_product(cameraPos, axis1b));

        cglib::vec3<double> origin = cameraPos * (coordScale * coordScale / cglib::norm(cameraPos));
        double radius = std::sqrt(std::max(0.0, coordScale * coordScale - cglib::norm(origin)));

        for (int i = 0; i <= tesselate; i++) {
            double u = 2.0 * Const::PI * (static_cast<double>(i < tesselate ? i : 0) / tesselate - 0.5);
            double x = std::cos(u);
            double y = std::sin(u);

            coords.emplace_back(cglib::vec3<float>::convert((axis1a * x + axis2a * y) * radius + (axis1b * x + axis2b * y) * height0 + origin - cameraPos));
            coords.emplace_back(cglib::vec3<float>::convert((axis1a * x + axis2a * y) * radius + (axis1b * x + axis2b * y) * height1 + origin - cameraPos));
            texCoords.emplace_back(0.5f, 0.0f);
            texCoords.emplace_back(0.5f, 1.0f);
        }
    }

    void BackgroundRenderer::BuildSphereSurface(std::vector<cglib::vec3<double> >& coords, std::vector<cglib::vec3<float> >& normals, std::vector<cglib::vec2<float> >& texCoords, std::vector<unsigned short>& indices, int tesselateU, int tesselateV) {
        int vertexCount = (tesselateU + 1) * (tesselateV + 1);
        int indexCount = 6 * tesselateU * tesselateV;
        coords.reserve(vertexCount);
        normals.reserve(vertexCount);
        texCoords.reserve(vertexCount);
        indices.reserve(indexCount);

        // Note: we use simple longitude/latitude tesselation scheme. Recursive tetrahedra-based surface would contain fewer vertexCount but produces texture artifacts near poles
        for (int j = 0; j <= tesselateV; j++) {
            float t = 1.0f - 1.0f * j / tesselateV;
            double v = Const::PI * (static_cast<double>(j) / tesselateV - 0.5);
            for (int i = 0; i <= tesselateU; i++) {
                float s = 2.0f * i / tesselateU;
                double u = 2.0 * Const::PI * (static_cast<double>(i < tesselateU ? i : 0) / tesselateU - 0.5);

                double x = std::cos(u) * std::cos(v);
                double y = std::sin(u) * std::cos(v);
                double z = std::sin(v);

                coords.emplace_back(x, y, z);
                normals.emplace_back(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                texCoords.emplace_back(s, t);
            }
        }

        for (int j = 0; j < tesselateV; j++) {
            for (int i = 0; i < tesselateU; i++) {
                int i00 = (i + 0) + (j + 0) * (tesselateU + 1);
                int i01 = (i + 0) + (j + 1) * (tesselateU + 1);
                int i10 = (i + 1) + (j + 0) * (tesselateU + 1);
                int i11 = (i + 1) + (j + 1) * (tesselateU + 1);

                indices.push_back(static_cast<unsigned short>(i00));
                indices.push_back(static_cast<unsigned short>(i10));
                indices.push_back(static_cast<unsigned short>(i01));
                indices.push_back(static_cast<unsigned short>(i10));
                indices.push_back(static_cast<unsigned short>(i11));
                indices.push_back(static_cast<unsigned short>(i01));
            }
        }
    }
    
    const float BackgroundRenderer::SKY_SCALE_MULTIPLIER_PLANAR = 2.0f / std::sqrt(3.0f);
    const float BackgroundRenderer::SKY_RELATIVE_HEIGHT_PLANAR[] = { 0.015625f, 0.0625f };
    const float BackgroundRenderer::SKY_HEIGHT_RAMP_PLANAR[] = { -40.0f, 12.0f };
    const float BackgroundRenderer::SKY_RELATIVE_HEIGHT_SPHERICAL[] = { -0.05f, 0.1f };
    const float BackgroundRenderer::SKY_HEIGHT_RAMP_SPHERICAL[2] = { 3.0f, 4.0f };

    const float BackgroundRenderer::PLANE_COORDS[] = {
        -0.5f, 0.5f, 0.0f, -0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f
    };

    const float BackgroundRenderer::PLANE_TEX_COORDS[] = {
        0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f
    };

    const std::string BackgroundRenderer::BACKGROUND_VERTEX_SHADER =
        "#version 100\n"
        "attribute vec3 a_coord;"
        "attribute vec3 a_normal;"
        "attribute vec2 a_texCoord;"
        "uniform vec3 u_lightDir;"
        "uniform mat4 u_mvpMat;"
        "varying vec4 v_color;"
        "varying vec2 v_texCoord;"
        "void main() {"
        "    float lighting = max(0.0, dot(a_normal, u_lightDir)) * 0.5 + 0.5;"
        "    v_color = vec4(lighting, lighting, lighting, 1.0);"
        "    v_texCoord = a_texCoord;"
        "    gl_Position = u_mvpMat * vec4(a_coord, 1.0);"
        "}";

    const std::string BackgroundRenderer::BACKGROUND_FRAGMENT_SHADER =
        "#version 100\n"
        "precision mediump float;"
        "uniform sampler2D u_tex;"
        "varying lowp vec4 v_color;"
        "\n#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
        "varying highp vec2 v_texCoord;"
        "\n#else\n"
        "varying mediump vec2 v_texCoord;"
        "\n#endif\n"
        "void main() {"
        "    vec4 color = texture2D(u_tex, v_texCoord) * v_color;"
        "    if (color.a == 0.0) {"
        "        discard;"
        "    }"
        "    gl_FragColor = color;"
        "}";
}
