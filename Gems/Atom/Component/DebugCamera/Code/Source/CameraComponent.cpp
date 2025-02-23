/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#include <Atom/Component/DebugCamera/CameraComponent.h>

#include <AzCore/Component/TransformBus.h>
#include <AzCore/IO/IOUtils.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Component/Entity.h>

#include <AzCore/Math/MatrixUtils.h>

#include <Atom/RHI/RHISystemInterface.h>

#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/AuxGeom/AuxGeomFeatureProcessorInterface.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/WindowContext.h>

namespace AZ
{
    namespace Debug
    {
        void CameraComponentConfig::Reflect(AZ::ReflectContext* context)
        {
            if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
            {
                serializeContext->Class<CameraComponentConfig, AZ::ComponentConfig>()
                    ->Version(1)
                    ->Field("FovY", &CameraComponentConfig::m_fovY)
                    ->Field("DepthNear", &CameraComponentConfig::m_depthNear)
                    ->Field("DepthFar", &CameraComponentConfig::m_depthFar)
                    ->Field("AspectRatioOverride", &CameraComponentConfig::m_aspectRatioOverride)
                    ;
            }
        }

        void CameraComponent::Reflect(AZ::ReflectContext* context)
        {
            CameraComponentConfig::Reflect(context);

            if (auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
            {
                serializeContext->Class<CameraComponent, AZ::Component>()
                    ->Version(1)
                    ->Field("Config", &CameraComponent::m_componentConfig)
                    ;
            }
        }

        void CameraComponent::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& required)
        {
            required.push_back(AZ_CRC("TransformService", 0x8ee22c50));
        }

        void CameraComponent::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
        {
            provided.push_back(AZ_CRC("CameraService", 0x1dd1caa4));
        }

        void CameraComponent::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible)
        {
            incompatible.push_back(AZ_CRC("CameraService", 0x1dd1caa4));
            incompatible.push_back(AZ_CRC_CE("NonUniformScaleService"));
        }

        void CameraComponent::Activate()
        {
            AZ::Name viewName = GetEntity() ?
                AZ::Name(AZStd::string::format("Camera View (entity: \"%s\")", GetEntity()->GetName().c_str())) :
                AZ::Name("Camera view (unknown entity)");
            
            uint32_t defaultViewIndex = static_cast<uint32_t>(RPI::ViewType::Default);
            if (defaultViewIndex < m_cameraViews.size())
            {
                m_cameraViews[defaultViewIndex] = RPI::View::CreateView(viewName, RPI::View::UsageCamera);
            }
            else
            {
                m_cameraViews.insert(m_cameraViews.begin() + defaultViewIndex, RPI::View::CreateView(viewName, RPI::View::UsageCamera));
            }

            // Add XR pipelines if it is active
            m_xrSystem = RPI::RPISystemInterface::Get()->GetXRSystem();
            if (m_xrSystem)
            {
                m_numXrViews = m_xrSystem->GetNumViews();
                AZ_Assert(m_numXrViews <= AZ::RPI::XRMaxNumViews, "Atom only supports two XR views");
                for (AZ::u32 i = 0; i < m_numXrViews; i++)
                {
                    uint32_t xrViewIndex =
                        i == 0 ? static_cast<uint32_t>(RPI::ViewType::XrLeft) : static_cast<uint32_t>(RPI::ViewType::XrRight);
                    if (xrViewIndex < m_cameraViews.size())
                    {
                        m_cameraViews[xrViewIndex] = RPI::View::CreateView(viewName, RPI::View::UsageCamera | RPI::View::UsageXR);
                    }
                    else
                    {
                        m_cameraViews.insert(m_cameraViews.begin() + xrViewIndex, RPI::View::CreateView(viewName, RPI::View::UsageCamera| RPI::View::UsageXR));
                    }
                }
            }

            for (uint16_t i = 0; i < AZ::RPI::XRMaxNumViews; i++)
            {
                if (i < m_stereoscopicViewQuats.size())
                {
                    m_stereoscopicViewQuats[i] = AZ::Quaternion::CreateIdentity();
                }
                else
                {
                    m_stereoscopicViewQuats.insert(m_stereoscopicViewQuats.begin() + i, AZ::Quaternion::CreateIdentity());
                }
            }

            m_auxGeomFeatureProcessor = RPI::Scene::GetFeatureProcessorForEntity<RPI::AuxGeomFeatureProcessorInterface>(GetEntityId());
            if (m_auxGeomFeatureProcessor)
            {
                m_auxGeomFeatureProcessor->GetOrCreateDrawQueueForView(m_cameraViews[defaultViewIndex].get());
            }

            //Get transform at start
            Transform transform;
            TransformBus::BroadcastResult(transform, &TransformBus::Events::GetWorldTM);

            OnTransformChanged(transform, transform);

            TransformNotificationBus::Handler::BusConnect(GetEntityId());
            RPI::ViewProviderBus::Handler::BusConnect(GetEntityId());
            Camera::CameraRequestBus::Handler::BusConnect(GetEntityId());
            Camera::CameraNotificationBus::Broadcast(&Camera::CameraNotificationBus::Events::OnCameraAdded, GetEntityId());
        }

        void CameraComponent::Deactivate()
        {
            Camera::CameraNotificationBus::Broadcast(&Camera::CameraNotificationBus::Events::OnCameraRemoved, GetEntityId());
            Camera::CameraRequestBus::Handler::BusDisconnect();
            RPI::ViewProviderBus::Handler::BusDisconnect();
            TransformNotificationBus::Handler::BusDisconnect();
            RPI::WindowContextNotificationBus::Handler::BusDisconnect();

            if (m_auxGeomFeatureProcessor)
            {
                m_auxGeomFeatureProcessor->ReleaseDrawQueueForView(m_cameraViews[static_cast<uint32_t>(RPI::ViewType::Default)].get());
            }
            
            for (auto& cameraView : m_cameraViews)
            {
                cameraView = nullptr;
            } 
            m_auxGeomFeatureProcessor = nullptr;
            m_stereoscopicViewQuats.clear();
            m_cameraViews.clear();
        }

        RPI::ViewPtr CameraComponent::GetView() const
        {
            AZ_Assert(aznumeric_cast<uint32_t>(m_cameraViews.size()) > 0, "No views available");
            return m_cameraViews[static_cast<uint32_t>(RPI::ViewType::Default)];
        }

        RPI::ViewPtr CameraComponent::GetStereoscopicView(RPI::ViewType viewType) const
        {
            uint32_t xrViewIndex = static_cast<uint32_t>(viewType);
            AZ_Assert(xrViewIndex < aznumeric_cast<uint32_t>(m_cameraViews.size()), "View with index %i does not exist", xrViewIndex);    
            return m_cameraViews[xrViewIndex];
        }

        bool CameraComponent::ReadInConfig(const AZ::ComponentConfig* baseConfig)
        {
            auto config = azrtti_cast<const CameraComponentConfig*>(baseConfig);

            if (config != nullptr)
            {
                m_componentConfig = *config;

                if (config->m_target != nullptr)
                {
                    RPI::WindowContextNotificationBus::Handler::BusConnect(m_componentConfig.m_target->GetWindowHandle());
                }

                UpdateAspectRatio();

                return true;
            }
            return false;
        }

        bool CameraComponent::WriteOutConfig(AZ::ComponentConfig* outBaseConfig) const
        {
            auto config = azrtti_cast<CameraComponentConfig*>(outBaseConfig);

            if (config != nullptr)
            {
                *config = m_componentConfig;
                return true;
            }
            return false;
        }

        void CameraComponent::OnTransformChanged([[maybe_unused]] const AZ::Transform& local, const AZ::Transform& world)
        {
            //Apply transform to stereoscopic views
            for (AZ::u32 i = 0; i < m_numXrViews; i++)
            {
                uint32_t xrViewIndex = i == 0 ? static_cast<uint32_t>(RPI::ViewType::XrLeft) : static_cast<uint32_t>(RPI::ViewType::XrRight);

                if (m_stereoscopicViewUpdate)
                {
                    // Apply the stereoscopic view provided by the device
                    AZ::Matrix3x4 worldTransform =
                        AZ::Matrix3x4::CreateFromQuaternionAndTranslation(m_stereoscopicViewQuats[i], world.GetTranslation());
                    m_cameraViews[xrViewIndex]->SetCameraTransform(worldTransform);
                }
                else
                {
                    // Apply the view using keyboard/mouse input
                    m_cameraViews[xrViewIndex]->SetCameraTransform(AZ::Matrix3x4::CreateFromTransform(world));
                }
            }

            // Apply transform to normal views
            if (m_stereoscopicViewUpdate)
            {
                //Handle the case when we have a PC window showing the view of the left eye
                AZ::Matrix3x4 worldTransform = AZ::Matrix3x4::CreateFromQuaternionAndTranslation(
                    m_stereoscopicViewQuats[static_cast<uint32_t>(RPI::ViewType::XrLeft)], world.GetTranslation());
                m_cameraViews[static_cast<uint32_t>(RPI::ViewType::Default)]->SetCameraTransform(worldTransform);
            }
            else
            {
                m_cameraViews[static_cast<uint32_t>(RPI::ViewType::Default)]->SetCameraTransform(AZ::Matrix3x4::CreateFromTransform(world));
            }
            m_stereoscopicViewUpdate = false;

            UpdateViewToClipMatrix();
        }

        float CameraComponent::GetFovDegrees()
        {
            return RadToDeg(m_componentConfig.m_fovY);
        }

        float CameraComponent::GetFovRadians()
        {
            return m_componentConfig.m_fovY;
        }

        float CameraComponent::GetNearClipDistance() 
        {
            return m_componentConfig.m_depthNear;
        }

        float CameraComponent::GetFarClipDistance()
        {
            return m_componentConfig.m_depthFar;
        }

        float CameraComponent::GetFrustumWidth()
        {
            return m_componentConfig.m_depthFar * tanf(m_componentConfig.m_fovY / 2) * m_aspectRatio * 2;
        }

        float CameraComponent::GetFrustumHeight()
        {
            return m_componentConfig.m_depthFar * tanf(m_componentConfig.m_fovY / 2) * 2;
        }

        bool CameraComponent::IsOrthographic()
        {
            return false;
        }

        float CameraComponent::GetOrthographicHalfWidth()
        {
            return 0.0f;
        }

        void CameraComponent::SetXRViewQuaternion(const AZ::Quaternion& viewQuat, uint32_t xrViewIndex)
        {
            AZ_Assert(xrViewIndex < AZ::RPI::XRMaxNumViews, "Xr view index is out of range.");
            m_stereoscopicViewQuats[xrViewIndex] = viewQuat;
            m_stereoscopicViewUpdate = true;
        }

        void CameraComponent::SetFovDegrees(float fov)
        {
            m_componentConfig.m_fovY = DegToRad(fov);
            UpdateViewToClipMatrix();
        }

        void CameraComponent::SetFovRadians(float fov)
        {
            m_componentConfig.m_fovY = fov;
            UpdateViewToClipMatrix();
        }

        void CameraComponent::SetNearClipDistance(float nearClipDistance)
        {
            m_componentConfig.m_depthNear = nearClipDistance;
            UpdateViewToClipMatrix();
        }

        void CameraComponent::SetFarClipDistance(float farClipDistance) 
        {
            m_componentConfig.m_depthFar = farClipDistance;
            UpdateViewToClipMatrix();
        }

        void CameraComponent::SetFrustumWidth(float width)
        {
            AZ_Assert(m_componentConfig.m_depthFar > 0.f, "Depth Far has to be positive.");
            AZ_Assert(m_aspectRatio > 0.f, "Aspect ratio must be positive.");
            const float height = width / m_aspectRatio;
            m_componentConfig.m_fovY = atanf(height / 2 / m_componentConfig.m_depthFar) * 2;
            UpdateViewToClipMatrix();
        }

        void CameraComponent::SetFrustumHeight(float height)
        {
            AZ_Assert(m_componentConfig.m_depthFar > 0.f, "Depth Far has to be positive.");
            m_componentConfig.m_fovY = atanf(height / 2 / m_componentConfig.m_depthFar) * 2;
            UpdateViewToClipMatrix();
        }

        void CameraComponent::SetOrthographic([[maybe_unused]] bool orthographic)
        {
            AZ_Assert(!orthographic, "DebugCamera does not support orthographic projection");
        }

        void CameraComponent::SetOrthographicHalfWidth([[maybe_unused]] float halfWidth)
        {
            AZ_Assert(false, "DebugCamera does not support orthographic projection");
        }

        void CameraComponent::MakeActiveView()
        {
            // do nothing
        }

        bool CameraComponent::IsActiveView()
        {
            return false;
        }

        AZ::Vector3 CameraComponent::ScreenToWorld([[maybe_unused]] const AZ::Vector2& screenPosition, [[maybe_unused]] float depth)
        {
            // not implemented
            return AZ::Vector3::CreateZero();
        }

        AZ::Vector3 CameraComponent::ScreenNdcToWorld([[maybe_unused]] const AZ::Vector2& screenPosition, [[maybe_unused]] float depth)
        {
            // not implemented
            return AZ::Vector3::CreateZero();
        }

        AZ::Vector2 CameraComponent::WorldToScreen([[maybe_unused]] const AZ::Vector3& worldPosition)
        {
            // not implemented
            return AZ::Vector2::CreateZero();
        }

        AZ::Vector2 CameraComponent::WorldToScreenNdc([[maybe_unused]] const AZ::Vector3& worldPosition)
        {
            // not implemented
            return AZ::Vector2::CreateZero();
        }

        void CameraComponent::OnViewportResized(uint32_t width, uint32_t height)
        {
            AZ_UNUSED(width);
            AZ_UNUSED(height);
            UpdateAspectRatio();
            UpdateViewToClipMatrix();
        }

        void CameraComponent::UpdateAspectRatio()
        {
            if (m_componentConfig.m_aspectRatioOverride > 0.0f)
            {
                m_aspectRatio = m_componentConfig.m_aspectRatioOverride;
            }
            else if (m_componentConfig.m_target)
            {
                const auto& viewport = m_componentConfig.m_target->GetViewport();
                if (viewport.m_maxX > 0.0f && viewport.m_maxY > 0.0f)
                {
                    m_aspectRatio = viewport.m_maxX / viewport.m_maxY;
                }
            }
        }

        void CameraComponent::UpdateViewToClipMatrix()
        {
            // O3de assumes a setup for reversed depth
            bool reverseDepth = true;
            
            AZ::Matrix4x4 viewToClipMatrix;
            MakePerspectiveFovMatrixRH(
                viewToClipMatrix,
                m_componentConfig.m_fovY,
                m_aspectRatio,
                m_componentConfig.m_depthNear,
                m_componentConfig.m_depthFar,
                reverseDepth);
            m_cameraViews[static_cast<uint32_t>(RPI::ViewType::Default)]->SetViewToClipMatrix(viewToClipMatrix);

            //Update stereoscopic projection matrix
            if (m_xrSystem)
            {
                AZ::Matrix4x4 projection = AZ::Matrix4x4::CreateIdentity();
                for (AZ::u32 i = 0; i < m_numXrViews; i++)
                {
                    uint32_t xrViewIndex = i == 0 ? static_cast<uint32_t>(RPI::ViewType::XrLeft) : static_cast<uint32_t>(RPI::ViewType::XrRight);
                    AZ::RPI::FovData fovData;
                    AZ::RPI::PoseData poseData;
                    [[maybe_unused]] AZ::RHI::ResultCode resultCode = m_xrSystem->GetViewFov(i, fovData);
                    resultCode = m_xrSystem->GetViewPose(i, poseData);

                    projection = m_xrSystem->CreateProjectionOffset(
                        fovData.m_angleLeft,
                        fovData.m_angleRight,
                        fovData.m_angleDown,
                        fovData.m_angleUp,
                        m_componentConfig.m_depthNear,
                        m_componentConfig.m_depthFar,
                        reverseDepth);
                    m_cameraViews[xrViewIndex]->SetStereoscopicViewToClipMatrix(projection, reverseDepth);
                }
            } 
        }

    } // namespace Debug
} // namespace AZ
